// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Containers/Ticker.h"
#include "LoreClient.h"

class SWidget;
class SWindow;
class UPackage;
class FObjectPostSaveContext;

/**
 * Editor module for the Lore Quick Commit toolbar integration.
 *
 * Owns:
 *   - The "Lore" section added to the Level Editor toolbar (status / settings / action).
 *   - The current ELoreStatus, refreshed on a timer and after every action.
 *   - The Slate panels (settings, commit, behind/diff) opened from the toolbar.
 */
class FLoreQuickCommitModule : public IModuleInterface
{
public:
	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Access the loaded module instance. */
	static FLoreQuickCommitModule& Get();

	/** Is the module currently loaded? */
	static bool IsAvailable();

	/** Current status driving the colored indicator. */
	ELoreStatus GetStatus() const { return CurrentStatus; }

	/** Color for the status indicator circle. */
	FLinearColor GetStatusColor() const;

	/** Short human-readable status word ("Synced", "Behind", ...). */
	FText GetStatusText() const;

	/** Kick off an asynchronous refresh of the status (health check + lore queries). */
	void RefreshStatusAsync();

private:
	// --- Toolbar -------------------------------------------------------------------
	void RegisterToolbar();
	TSharedRef<SWidget> BuildStatusIndicatorWidget();
	void OnSettingsButtonClicked();
	void OnActionButtonClicked();

	// --- Panels (Slate windows) ----------------------------------------------------
	void OpenSettingsPanel();
	void OpenCommitPanel();
	void OpenBehindPanel(const FString& DiffSummary);

	/** Modal prompt for just the server IP/hostname. Returns false if cancelled. */
	bool PromptForServerHost(FString& OutHost);

	// --- Status helpers ------------------------------------------------------------
	void SetStatus(ELoreStatus NewStatus);
	bool OnPollTimer(float DeltaTime);

	/** Fired by the editor whenever a package (asset or level) is saved. Triggers an
	 *  immediate status refresh so the indicator turns YELLOW as soon as the user saves. */
	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext SaveContext);

	/** True if any unsaved (dirty) packages exist. */
	static bool HasUnsavedChanges();
	/** Show the standard save-dirty-packages prompt. Returns false if user cancelled. */
	static bool PromptSaveChanges();

	// --- State ---------------------------------------------------------------------
	ELoreStatus CurrentStatus = ELoreStatus::Disconnected;

	/** Last known server reachability (from HTTP health check). */
	bool bServerReachable = false;
	/** Last known local-repo initialisation state. */
	bool bRepoInitialized = false;
	/** Guards against overlapping async refreshes. */
	bool bRefreshInFlight = false;

	/** Handle for the periodic poll ticker. */
	FTSTicker::FDelegateHandle PollTickerHandle;

	/** Handle for the package-saved delegate (save -> immediate refresh). */
	FDelegateHandle PackageSavedHandle;

	/** Weak handle to the currently-open settings window (so we don't open duplicates). */
	TWeakPtr<SWindow> SettingsWindow;
};
