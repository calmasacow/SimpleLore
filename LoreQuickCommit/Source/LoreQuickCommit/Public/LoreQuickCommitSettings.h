// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LoreQuickCommitSettings.generated.h"

/**
 * Per-user configuration for the Lore Quick Commit plugin.
 *
 * Deliberately stored in the PER-USER config layer (Saved/Config/.../
 * EditorPerProjectUserSettings.ini), which lives under Saved/ and is excluded from
 * version control. The Lore server address differs per developer (LAN IP, VPN, remote
 * host), so it must NOT travel with the project — each user enters their own server the
 * first time they open the project. Also appears under Project Settings > Plugins.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Lore Quick Commit"))
class LOREQUICKCOMMIT_API ULoreQuickCommitSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	ULoreQuickCommitSettings();

	//~ UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** Master switch for whether Lore version control is active for this project. */
	UPROPERTY(config, EditAnywhere, Category = "Lore")
	bool bEnabled = true;

	/**
	 * The Lore server host: just the IP address or hostname (no port, no scheme).
	 * The plugin appends the appropriate ports (gRPC/QUIC + HTTP health-check).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Lore")
	FString ServerHost;

	/**
	 * Set to true once "Initialize Project" has successfully created and pushed the
	 * repository. This is a hint only; the live status is re-verified at runtime by
	 * checking for the local .lore directory.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Lore")
	bool bInitialized = false;

	/**
	 * Path to the lore executable. Leave as "lore.exe" to resolve it from the system
	 * PATH, or set an absolute path (e.g. C:/Users/you/bin/lore.exe).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Lore|Advanced")
	FString LoreExecutable = TEXT("lore.exe");

	/** gRPC/QUIC port used in the lore:// remote URL. */
	UPROPERTY(config, EditAnywhere, Category = "Lore|Advanced")
	int32 ServerPort = 41337;

	/** HTTP port used for the /health_check connectivity probe. */
	UPROPERTY(config, EditAnywhere, Category = "Lore|Advanced")
	int32 HealthCheckPort = 41339;

	/** How often (seconds) to poll the server for newer commits. Clamped to [30, 120]. */
	UPROPERTY(config, EditAnywhere, Category = "Lore|Advanced", meta = (ClampMin = "30", ClampMax = "120"))
	float PollIntervalSeconds = 45.0f;

	/** Convenience accessor for the mutable settings singleton. */
	static ULoreQuickCommitSettings* GetMutable();

	/** Convenience accessor for the const settings singleton. */
	static const ULoreQuickCommitSettings* Get();

	/** Persist the current values back to the config file on disk. */
	void SaveToConfig();
};
