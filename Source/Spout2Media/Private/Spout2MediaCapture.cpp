// Fill out your copyright notice in the Description page of Project Settings.


#include "Spout2MediaCapture.h"

#include <string>

#include "RHICommandList.h"
#include "RenderingThread.h"
#include "ID3D12DynamicRHI.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11on12.h>
#include "Spout.h"
#include "Windows/HideWindowsPlatformTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpout2Media, Log, All);

namespace
{
	/**
	 * D3D12 interop objects shared by every sender context.
	 *
	 * On D3D12 the Spout shared texture lives on a D3D11On12 device. That device submits its work to
	 * its own command queue, so a pair of fences is used to order the copy against the RHI's queue:
	 *   RHI queue   : ... render frame ... Signal(RHIToSpoutFence)             Wait(SpoutToRHIFence) ...
	 *   Spout queue :                      Wait(RHIToSpoutFence) Copy Flush    Signal(SpoutToRHIFence)
	 *
	 * The RHI command stream keeps raw pointers to these fences, so they must outlive every sender
	 * context. They are created on first use and released when the module shuts down.
	 */
	struct FSpoutD3D12Interop
	{
		TRefCountPtr<ID3D12CommandQueue> Queue;
		TRefCountPtr<ID3D12Fence> RHIToSpoutFence;
		TRefCountPtr<ID3D12Fence> SpoutToRHIFence;
		uint64 FenceValue = 0;
		HANDLE WaitEvent = nullptr;

		bool Initialize(ID3D12Device* Device)
		{
			if (Queue)
			{
				return true;
			}

			D3D12_COMMAND_QUEUE_DESC QueueDesc = {};
			QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

			if (FAILED(Device->CreateCommandQueue(&QueueDesc, IID_PPV_ARGS(Queue.GetInitReference())))
				|| FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(RHIToSpoutFence.GetInitReference())))
				|| FAILED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(SpoutToRHIFence.GetInitReference()))))
			{
				UE_LOG(LogSpout2Media, Error, TEXT("Failed to create the D3D12 interop queue/fences for Spout."));
				Shutdown();
				return false;
			}

			Queue->SetName(L"Spout2Media Interop Queue");
			RHIToSpoutFence->SetName(L"Spout2Media RHI->Spout Fence");
			SpoutToRHIFence->SetName(L"Spout2Media Spout->RHI Fence");
			WaitEvent = CreateEvent(nullptr, 0, 0, nullptr);
			return true;
		}

		/** Blocks the CPU until the interop queue has executed every copy issued so far. */
		void WaitForIdle()
		{
			if (SpoutToRHIFence && WaitEvent && SpoutToRHIFence->GetCompletedValue() < FenceValue)
			{
				if (SUCCEEDED(SpoutToRHIFence->SetEventOnCompletion(FenceValue, WaitEvent)))
				{
					WaitForSingleObject(WaitEvent, 2000);
				}
			}
		}

		void Shutdown()
		{
			WaitForIdle();
			if (WaitEvent)
			{
				CloseHandle(WaitEvent);
				WaitEvent = nullptr;
			}
			SpoutToRHIFence.SafeRelease();
			RHIToSpoutFence.SafeRelease();
			Queue.SafeRelease();
		}
	};

	FSpoutD3D12Interop GSpoutD3D12Interop;
}

void Spout2Media_ShutdownD3D12Interop()
{
	if (GSpoutD3D12Interop.Queue)
	{
		if (IsInGameThread() && GDynamicRHI && GIsRHIInitialized)
		{
			// Make sure no RHI payload still references our fences before they are released.
			FlushRenderingCommands();
			GDynamicRHI->RHIBlockUntilGPUIdle();
		}
		GSpoutD3D12Interop.Shutdown();
	}
}

struct USpout2MediaCapture::FSpoutSenderContext
{
	FString SenderName;
	uint32 Width, Height;
	EPixelFormat PixelFormat;

	std::string SenderName_str;

	bool bIsD3D12 = false;
	bool bInitialized = false;
	bool bSenderCreated = false;

	ID3D11Device* D3D11Device = nullptr;
	ID3D11DeviceContext* DeviceContext = nullptr;

	ID3D11On12Device* D3D11on12Device = nullptr;
	TMap<FTextureRHIRef, ID3D11Resource*> WrappedDX11ResourceMap;

	spoutSenderNames senders;
	spoutDirectX sdx;

	ID3D11Texture2D* SendingTexture = nullptr;
	HANDLE SharedSendingHandle = nullptr;

	FSpoutSenderContext(const FString& SenderName,
		uint32 Width, uint32 Height, EPixelFormat PixelFormat,
		FTextureRHIRef InTexture)
		: SenderName(SenderName)
		, Width(Width)
		, Height(Height)
		, PixelFormat(PixelFormat)
	{
		SenderName_str = TCHAR_TO_ANSI(*SenderName);
		bInitialized = InitSpout(InTexture);
	}

	~FSpoutSenderContext()
	{
		DisposeSpout();
	}

	bool InitSpout(FTextureRHIRef InTexture)
	{
		const FString RHIName = GDynamicRHI->GetName();
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;

		if (RHIName == TEXT("D3D11"))
		{
			// Same device and immediate context as the RHI, so our copy is naturally ordered with the RHI's work.
			D3D11Device = static_cast<ID3D11Device*>(GDynamicRHI->RHIGetNativeDevice());
			D3D11Device->AddRef(); // Balanced by the Release() in DisposeSpout.
			D3D11Device->GetImmediateContext(&DeviceContext);

			ID3D11Texture2D* NativeTex = static_cast<ID3D11Texture2D*>(InTexture->GetNativeResource());
			D3D11_TEXTURE2D_DESC Desc;
			NativeTex->GetDesc(&Desc);
			Format = Desc.Format;
		}
		else if (RHIName == TEXT("D3D12"))
		{
			bIsD3D12 = true;

			ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
			ID3D12Device* Device12 = RHI->RHIGetDevice(0);

			if (!GSpoutD3D12Interop.Initialize(Device12))
			{
				return false;
			}

			// The D3D11On12 device submits to our own queue so that we can fence it against the RHI's queue.
			IUnknown* Queues[] = { GSpoutD3D12Interop.Queue.GetReference() };
			const UINT DeviceFlags11 = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

			HRESULT Result = D3D11On12CreateDevice(
				Device12,
				DeviceFlags11,
				nullptr, 0,
				Queues, UE_ARRAY_COUNT(Queues),
				0,
				&D3D11Device,
				&DeviceContext,
				nullptr);

			if (FAILED(Result))
			{
				UE_LOG(LogSpout2Media, Error, TEXT("D3D11On12CreateDevice failed (0x%08x)."), Result);
				return false;
			}

			Result = D3D11Device->QueryInterface(IID_PPV_ARGS(&D3D11on12Device));
			if (FAILED(Result))
			{
				UE_LOG(LogSpout2Media, Error, TEXT("Failed to query ID3D11On12Device (0x%08x)."), Result);
				return false;
			}

			Format = RHI->RHIGetResource(InTexture)->GetDesc().Format;
		}
		else
		{
			UE_LOG(LogSpout2Media, Error, TEXT("Spout output is only supported on D3D11 and D3D12 (current RHI: %s)."), *RHIName);
			return false;
		}

		if (!senders.CreateSender(SenderName_str.c_str(), Width, Height, SharedSendingHandle, Format))
		{
			UE_LOG(LogSpout2Media, Error, TEXT("Failed to create Spout sender '%s'."), *SenderName);
			return false;
		}
		bSenderCreated = true;

		if (!sdx.CreateSharedDX11Texture(D3D11Device, Width, Height, Format, &SendingTexture, SharedSendingHandle))
		{
			UE_LOG(LogSpout2Media, Error, TEXT("Failed to create the Spout shared texture for '%s' (%ux%u, DXGI format %d)."), *SenderName, Width, Height, static_cast<int32>(Format));
			return false;
		}

		UE_LOG(LogSpout2Media, Log, TEXT("Spout sender '%s' started (%ux%u, DXGI format %d, %s)."), *SenderName, Width, Height, static_cast<int32>(Format), *RHIName);
		return true;
	}

	void DisposeSpout()
	{
		if (bIsD3D12)
		{
			if (IsInRenderingThread())
			{
				// Make sure the RHI-side fence signal has been dispatched before we wait on our queue.
				FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
			}
			// Our queue may still be reading the wrapped RHI textures; wait for it before releasing the wrappers.
			GSpoutD3D12Interop.WaitForIdle();
		}

		for (auto& Pair : WrappedDX11ResourceMap)
		{
			if (Pair.Value)
			{
				Pair.Value->Release();
			}
		}
		WrappedDX11ResourceMap.Reset();

		if (bSenderCreated)
		{
			senders.ReleaseSenderName(SenderName_str.c_str());
			bSenderCreated = false;
		}

		if (SendingTexture)
		{
			SendingTexture->Release();
			SendingTexture = nullptr;
		}

		if (DeviceContext)
		{
			DeviceContext->Release();
			DeviceContext = nullptr;
		}

		if (D3D11on12Device)
		{
			D3D11on12Device->Release();
			D3D11on12Device = nullptr;
		}

		if (D3D11Device)
		{
			D3D11Device->Release();
			D3D11Device = nullptr;
		}
	}

	/** D3D12 only: returns (and caches) the D3D11On12 wrapper for an RHI texture. The wrapper is acquired/released in COPY_SOURCE. */
	ID3D11Resource* GetWrappedResource(ID3D12DynamicRHI* RHI, FTextureRHIRef InTexture)
	{
		if (ID3D11Resource** Found = WrappedDX11ResourceMap.Find(InTexture))
		{
			return *Found;
		}

		ID3D12Resource* NativeTex = RHI->RHIGetResource(InTexture);
		D3D11_RESOURCE_FLAGS Flags11 = {};
		ID3D11Resource* Wrapped = nullptr;

		const HRESULT Result = D3D11on12Device->CreateWrappedResource(
			NativeTex, &Flags11,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			IID_PPV_ARGS(&Wrapped));

		if (FAILED(Result))
		{
			UE_LOG(LogSpout2Media, Error, TEXT("CreateWrappedResource failed (0x%08x)."), Result);
			return nullptr;
		}

		WrappedDX11ResourceMap.Add(InTexture, Wrapped);
		return Wrapped;
	}

	void Tick_RenderThread(FRHICommandListImmediate& RHICmdList, FTextureRHIRef InTexture)
	{
		if (!bInitialized || !DeviceContext || !SendingTexture)
		{
			return;
		}

		if (!bIsD3D12)
		{
			ID3D11Texture2D* SourceTexture = static_cast<ID3D11Texture2D*>(InTexture->GetNativeResource());
			DeviceContext->CopyResource(SendingTexture, SourceTexture);
			DeviceContext->Flush();
		}
		else
		{
			ID3D12DynamicRHI* RHI = GetID3D12DynamicRHI();
			ID3D11Resource* Wrapped = GetWrappedResource(RHI, InTexture);
			if (!Wrapped)
			{
				return;
			}

			// 1. Put the captured texture into COPY_SOURCE through the RHI so its state tracking stays consistent
			//    with what the D3D11On12 wrapper assumes.
			RHICmdList.Transition(FRHITransitionInfo(InTexture, ERHIAccess::Unknown, ERHIAccess::CopySrc));

			// 2. The RHI queue signals once the frame (and the transition above) has executed on the GPU.
			const uint64 FenceValue = ++GSpoutD3D12Interop.FenceValue;
			ID3D12Fence* RHIToSpout = GSpoutD3D12Interop.RHIToSpoutFence;
			ID3D12Fence* SpoutToRHI = GSpoutD3D12Interop.SpoutToRHIFence;

			RHICmdList.EnqueueLambda(TEXT("Spout2Media_SignalFrameReady"), [RHIToSpout, FenceValue](FRHICommandListBase& ExecutingCmdList)
			{
				GetID3D12DynamicRHI()->RHISignalManualFence(static_cast<FRHICommandList&>(ExecutingCmdList), RHIToSpout, FenceValue);
			});

			// 3. Our queue waits for that signal, then copies into the Spout shared texture.
			GSpoutD3D12Interop.Queue->Wait(RHIToSpout, FenceValue);

			D3D11on12Device->AcquireWrappedResources(&Wrapped, 1);
			DeviceContext->CopyResource(SendingTexture, Wrapped);
			D3D11on12Device->ReleaseWrappedResources(&Wrapped, 1);
			DeviceContext->Flush();

			TSharedPtr<std::atomic<bool>> bHasSignalled = MakeShared<std::atomic<bool>>(false);
			GSpoutD3D12Interop.Queue->Signal(SpoutToRHI, FenceValue);
			*bHasSignalled = true;

			// 4. The RHI queue must not write to this texture again until our copy has finished.
			RHICmdList.EnqueueLambda(TEXT("Spout2Media_WaitCopyDone"), [SpoutToRHI, FenceValue, bHasSignalled](FRHICommandListBase& ExecutingCmdList)
			{
				GetID3D12DynamicRHI()->RHIWaitManualFence(static_cast<FRHICommandList&>(ExecutingCmdList), SpoutToRHI, FenceValue, bHasSignalled);
			});
		}

		senders.UpdateSender(SenderName_str.c_str(), Width, Height, SharedSendingHandle);
	}
};

USpout2MediaCapture::USpout2MediaCapture()
	: Super()
{
}

bool USpout2MediaCapture::HasFinishedProcessing() const
{
	return Super::HasFinishedProcessing();
}

bool USpout2MediaCapture::ValidateMediaOutput() const
{
	USpout2MediaOutput* Output = CastChecked<USpout2MediaOutput>(MediaOutput);
	check(Output);
	return true;
}

bool USpout2MediaCapture::InitializeCapture()
{
	USpout2MediaOutput* Output = CastChecked<USpout2MediaOutput>(MediaOutput);
	check(Output);
	return InitSpout(Output);
}

bool USpout2MediaCapture::UpdateSceneViewportImpl(TSharedPtr<FSceneViewport>& InSceneViewport)
{
	return true;
}

void USpout2MediaCapture::StopCaptureImpl(bool bAllowPendingFrameToBeProcess)
{
	Super::StopCaptureImpl(bAllowPendingFrameToBeProcess);
	DisposeSpout();
}

void USpout2MediaCapture::OnRHIResourceCaptured_RenderingThread(FRHICommandListImmediate& RHICmdList, const FCaptureBaseData& InBaseData,
	TSharedPtr<FMediaCaptureUserData, ESPMode::ThreadSafe> InUserData, FTextureRHIRef InTexture)
{
	USpout2MediaOutput* Output = CastChecked<USpout2MediaOutput>(MediaOutput);

	const FString SenderName = Output->SenderName;
	auto InTexture2D = InTexture->GetTexture2D();
	uint32 Width = InTexture2D->GetSizeX();
	uint32 Height = InTexture2D->GetSizeY();
	EPixelFormat PixelFormat = InTexture2D->GetFormat();

	if (!Context
		|| Context->SenderName != SenderName
		|| Context->Width != Width
		|| Context->Height != Height
		|| Context->PixelFormat != PixelFormat)
	{
		if (Context)
			Context.Reset();

		Context = MakeShared<FSpoutSenderContext, ESPMode::ThreadSafe>(
			SenderName, Width, Height, PixelFormat, InTexture);
	}

	if (Context)
	{
		Context->Tick_RenderThread(RHICmdList, InTexture);
	}
}

//////////////////////////////////////////////////////////////////////////////

bool USpout2MediaCapture::InitSpout(USpout2MediaOutput* Output)
{
	SetState(EMediaCaptureState::Capturing);
	return true;
}

bool USpout2MediaCapture::DisposeSpout()
{
	SetState(EMediaCaptureState::Stopped);

	Context.Reset();
	return true;
}
