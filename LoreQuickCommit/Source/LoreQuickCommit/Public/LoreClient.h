// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// Shared log category for the whole plugin.
DECLARE_LOG_CATEGORY_EXTERN(LogLoreQuickCommit, Log, All);

/**
 * High-level synchronisation state shown by the toolbar status indicator.
 *
 *   Disconnected (RED)    - No connection to the Lore server, OR the project is not
 *                           initialised with Lore.
 *   Synced       (GREEN)  - Initialised and fully in sync with the server.
 *   LocalChanges (YELLOW) - There are local uncommitted changes.
 *   Behind       (ORANGE) - The remote has newer commits than the local project.
 */
enum class ELoreStatus : uint8
{
	Disconnected,
	Synced,
	LocalChanges,
	Behind
};

/** Result of running a single lore command. */
struct FLoreResult
{
	/** True if the process launched AND returned exit code 0. */
	bool bSuccess = false;

	/** Process exit code (0 == success for most CLIs). */
	int32 ReturnCode = -1;

	/** Captured stdout. */
	FString Output;

	/** Captured stderr. */
	FString Error;

	/** Combined stdout + stderr, handy for display. */
	FString CombinedOutput() const
	{
		FString Combined = Output;
		if (!Error.IsEmpty())
		{
			if (!Combined.IsEmpty())
			{
				Combined += TEXT("\n");
			}
			Combined += Error;
		}
		return Combined;
	}
};

/**
 * Thin, synchronous wrapper around the `lore` command-line tool plus a few helpers
 * for deriving status.
 *
 * IMPORTANT: The exact lore sub-commands and the way their text output is parsed are
 * centralised here so they are trivial to adjust as the lore CLI evolves. Every place
 * that shells out to lore lives in this file.
 *
 * These calls are BLOCKING. The module is responsible for running the slower ones on a
 * background thread (see FLoreQuickCommitModule::RefreshStatusAsync).
 */
class LOREQUICKCOMMIT_API FLoreClient
{
public:
	// --- Environment helpers --------------------------------------------------------

	/** Resolved lore executable (from settings; "lore.exe" resolves via PATH). */
	static FString GetExecutable();

	/** Absolute path to the project directory (where the .uproject lives) — lore's cwd. */
	static FString GetWorkingDirectory();

	/** Current project name, used as the repository name on the server. */
	static FString GetProjectName();

	/** Build the lore:// remote URL for a host, e.g. lore://192.168.1.200:41337/MyProject */
	static FString BuildRemoteUrl(const FString& Host);

	/** Build the HTTP health-check URL for a host, e.g. http://192.168.1.200:41339/health_check */
	static FString BuildHealthCheckUrl(const FString& Host);

	// --- Raw command runner ---------------------------------------------------------

	/** Run `lore <Args>` in the project directory and capture its output. Blocking. */
	static FLoreResult Run(const FString& Args);

	// --- Status queries -------------------------------------------------------------

	/** True if this project already has a local lore repository (a .lore directory). */
	static bool IsRepoInitialized();

	/** True if `lore status` reports local, uncommitted changes. */
	static bool HasLocalChanges();

	/** Fetch from the server and report whether the remote has commits we don't have.
	 *  @param OutSummary  Receives a human-readable summary of the differences. */
	static bool IsBehindRemote(FString& OutSummary);

	// --- Actions --------------------------------------------------------------------

	/**
	 * Write a default .loreignore (Unreal generated/temporary folders) at the project root
	 * if one isn't already present. Returns true if the file exists afterwards. Called by
	 * InitializeProject before the first scan so generated files never get staged.
	 */
	static bool EnsureLoreIgnore();

	/**
	 * Initialise the project against the server:
	 *   ensure .loreignore -> repository create (if needed) -> stage . --scan ->
	 *   commit "Initial Commit" -> push
	 * @return The result of the final push (or the failing earlier step).
	 */
	static FLoreResult InitializeProject(const FString& Host);

	/** Commit & publish local changes:  lore stage . -> lore commit "<Message>" -> lore push */
	static FLoreResult CommitAndPush(const FString& Message);

	/** Destructively overwrite local files with the server version:  lore pull (force). */
	static FLoreResult PullLatest();

private:
	/** Escape a commit message so it can be passed as a single quoted argument. */
	static FString SanitizeMessage(const FString& Message);
};
