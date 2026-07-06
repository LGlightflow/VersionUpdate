// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Version/PatchManifest.h"
#include "VersionManifestObject.generated.h"

/**
 * 
 */
UCLASS()
class VERSIONUPDATE_API UVersionManifestObject : public UObject
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere,Category = "Editor Version Manifest")
	FString DomainOrIP = "127.0.0.1:81";

	UPROPERTY(EditAnywhere, Category = "Editor Version Manifest")
	FString BucketName = "Test";

	UPROPERTY(EditAnywhere, Category = "Editor Version Manifest")
	FString Platform = "Windows";

	UPROPERTY(EditAnywhere, Category = "Editor Version Manifest")
	FPatchList PatchList;

};
