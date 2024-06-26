﻿// Fill out your copyright notice in the Description page of Project Settings.


#include "Spout2MediaCapture.h"

#include "Windows/AllowWindowsPlatformTypes.h" 
#include <d3d11on12.h>
#include "Spout.h"
#include "Windows/HideWindowsPlatformTypes.h"

struct USpout2MediaCapture::FSpoutSenderContext
{
	FString SenderName;
	uint32 Width, Height;
	EPixelFormat PixelFormat;

	std::string SenderName_str;

	ID3D11DeviceContext* DeviceContext = nullptr;
	ID3D11Device* D3D11Device = nullptr;
	
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
		InitSpout(InTexture);
	}

	~FSpoutSenderContext()
	{
		DisposeSpout();
	}

	void InitSpout(FTextureRHIRef InTexture)
	{
		const FString RHIName = GDynamicRHI->GetName();

		if (RHIName == TEXT("D3D11"))
		{
			D3D11Device = static_cast<ID3D11Device*>(GDynamicRHI->RHIGetNativeDevice());
			D3D11Device->GetImmediateContext(&DeviceContext);
		}
		else if (RHIName == TEXT("D3D12"))
		{
			ID3D12Device* Device12 = static_cast<ID3D12Device*>(GDynamicRHI->RHIGetNativeDevice());
			UINT DeviceFlags11 = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

			verify(D3D11On12CreateDevice(
				Device12,
				DeviceFlags11,
				nullptr,
				0,
				nullptr,
				0,
				0,
				&D3D11Device,
				&DeviceContext,
				nullptr
			) == S_OK);

			verify(D3D11Device->QueryInterface(__uuidof(ID3D11On12Device), (void**)&D3D11on12Device) == S_OK);
		}

		ID3D12Resource* NativeTex = (ID3D12Resource*)InTexture->GetNativeResource();
		D3D12_RESOURCE_DESC desc = NativeTex->GetDesc();
		
		verify(senders.CreateSender(SenderName_str.c_str(), Width, Height, SharedSendingHandle, desc.Format));
		verify(sdx.CreateSharedDX11Texture(D3D11Device, Width, Height, desc.Format, &SendingTexture, SharedSendingHandle));
	}

	void DisposeSpout()
	{
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

		{
			for (auto Iter : WrappedDX11ResourceMap)
			{
				D3D11on12Device->ReleaseWrappedResources(&Iter.Value, 1);
			}
			WrappedDX11ResourceMap.Reset();
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

	ID3D11Texture2D* GetTextureResource(FTextureRHIRef InTexture)
	{
		const FString RHIName = GDynamicRHI->GetName();
		
		if (RHIName == TEXT("D3D11"))
		{
			return static_cast<ID3D11Texture2D*>(InTexture->GetNativeResource());
		}
		else if (RHIName == TEXT("D3D12"))
		{
			if (auto Iter = WrappedDX11ResourceMap.Find(InTexture))
				return static_cast<ID3D11Texture2D*>(*Iter);

			ID3D12Resource* NativeTex = static_cast<ID3D12Resource*>(InTexture->GetNativeResource());
			ID3D11Resource* WrappedDX11Resource = nullptr;
			
			D3D11_RESOURCE_FLAGS rf11 = {};
			verify(D3D11on12Device->CreateWrappedResource(
				NativeTex, &rf11,
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource),
				(void**)&WrappedDX11Resource) == S_OK);

			NativeTex->Release();

			WrappedDX11ResourceMap.Add(InTexture, WrappedDX11Resource);

			return static_cast<ID3D11Texture2D*>(WrappedDX11Resource);
		}
		
		return nullptr;
	}

	void Tick_RenderThread(FTextureRHIRef InTexture)
	{
		const FString RHIName = GDynamicRHI->GetName();

		if (!DeviceContext)
			return;

		auto Texture = GetTextureResource(InTexture);
		DeviceContext->CopyResource(SendingTexture, Texture);
		DeviceContext->Flush();
		
		verify(senders.UpdateSender(SenderName_str.c_str(),
			Width, Height,
			SharedSendingHandle));
	}
};

USpout2MediaCapture::USpout2MediaCapture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
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

void USpout2MediaCapture::OnRHIResourceCaptured_RenderingThread(const FCaptureBaseData& InBaseData,
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
		Context->Tick_RenderThread(InTexture);
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
