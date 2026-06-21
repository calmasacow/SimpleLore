// Copyright Epic Games, Inc. All Rights Reserved.

#include "LoreClient.h"
#include "LoreQuickCommitSettings.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

DEFINE_LOG_CATEGORY(LogLoreQuickCommit);

namespace
{
	/**
	 * Resolve a bare executable filename (e.g. "lore.exe") to a full path.
	 *
	 * We can't rely on the OS searching PATH: the editor process inherits its environment
	 * from whatever launched it (Explorer / Epic Launcher / IDE), and that PATH is often
	 * stale — it may not contain a directory that was added to the user's PATH afterwards.
	 * So we resolve it ourselves: scan the live PATH directories first, then fall back to
	 * the user's ~/bin. Returns the bare name if nothing matches (let the OS try).
	 */
	FString ResolveExecutablePath(const FString& FileName)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		// 1) Every directory on the process PATH.
		const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
		TArray<FString> Dirs;
		PathEnv.ParseIntoArray(Dirs, TEXT(";"), /*CullEmpty*/ true);
		for (const FString& Dir : Dirs)
		{
			const FString Candidate = FPaths::Combine(Dir.TrimQuotes(), FileName);
			if (PlatformFile.FileExists(*Candidate))
			{
				return FPaths::ConvertRelativePathToFull(Candidate);
			}
		}

		// 2) Common fallback: the current user's home "bin" directory.
		const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (!UserProfile.IsEmpty())
		{
			const FString Candidate = FPaths::Combine(UserProfile, TEXT("bin"), FileName);
			if (PlatformFile.FileExists(*Candidate))
			{
				return FPaths::ConvertRelativePathToFull(Candidate);
			}
		}

		// 3) Couldn't locate it — return the bare name and let ExecProcess/OS try PATH.
		return FileName;
	}

	/**
	 * Extract the first integer that appears after Marker in Text (skipping spaces).
	 * Returns -1 if Marker isn't found or no number follows. Used to read the revision
	 * numbers out of `lore status`, e.g.:
	 *   "On branch main revision 7 -> <hash>"  ->  ExtractIntAfter(out, "revision ")        == 7
	 *   "Remote revision 9 -> <hash>"          ->  ExtractIntAfter(out, "Remote revision ") == 9
	 */
	int64 ExtractIntAfter(const FString& Text, const FString& Marker)
	{
		const int32 Idx = Text.Find(Marker, ESearchCase::IgnoreCase);
		if (Idx == INDEX_NONE)
		{
			return -1;
		}

		FString Number;
		for (int32 i = Idx + Marker.Len(); i < Text.Len(); ++i)
		{
			const TCHAR Ch = Text[i];
			if (FChar::IsDigit(Ch))
			{
				Number.AppendChar(Ch);
			}
			else if (Number.Len() > 0)
			{
				break; // reached the end of the number
			}
			else if (Ch == TEXT(' '))
			{
				continue; // skip leading spaces between marker and number
			}
			else
			{
				break; // a non-space, non-digit before any digit
			}
		}
		return Number.IsEmpty() ? -1 : FCString::Atoi64(*Number);
	}
}

// ---------------------------------------------------------------------------------------
// Environment helpers
// ---------------------------------------------------------------------------------------

FString FLoreClient::GetExecutable()
{
	const ULoreQuickCommitSettings* Settings = ULoreQuickCommitSettings::Get();
	FString Configured = Settings ? Settings->LoreExecutable : FString();
	if (Configured.IsEmpty())
	{
		Configured = TEXT("lore.exe");
	}

	// An explicit path (contains a separator) is trusted as-is.
	if (Configured.Contains(TEXT("/")) || Configured.Contains(TEXT("\\")))
	{
		return Configured;
	}

	// A bare filename gets resolved against PATH + ~/bin (see note above).
	return ResolveExecutablePath(Configured);
}

FString FLoreClient::GetWorkingDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

FString FLoreClient::GetProjectName()
{
	// FApp::GetProjectName() returns the short project name (e.g. "LorePlugin").
	return FApp::GetProjectName();
}

FString FLoreClient::BuildRemoteUrl(const FString& Host)
{
	const ULoreQuickCommitSettings* Settings = ULoreQuickCommitSettings::Get();
	const int32 Port = Settings ? Settings->ServerPort : 41337;
	return FString::Printf(TEXT("lore://%s:%d/%s"), *Host, Port, *GetProjectName());
}

FString FLoreClient::BuildHealthCheckUrl(const FString& Host)
{
	const ULoreQuickCommitSettings* Settings = ULoreQuickCommitSettings::Get();
	const int32 Port = Settings ? Settings->HealthCheckPort : 41339;
	return FString::Printf(TEXT("http://%s:%d/health_check"), *Host, Port);
}

// ---------------------------------------------------------------------------------------
// Raw command runner
// ---------------------------------------------------------------------------------------

FLoreResult FLoreClient::Run(const FString& Args)
{
	FLoreResult Result;

	const FString Exe = GetExecutable();
	const FString WorkingDir = GetWorkingDirectory();

	UE_LOG(LogLoreQuickCommit, Log, TEXT("Running: \"%s\" %s   (cwd: %s)"), *Exe, *Args, *WorkingDir);

	// ExecProcess is synchronous: it blocks until the child process exits. On Windows the
	// executable name is resolved against PATH when no directory component is supplied.
	const bool bLaunched = FPlatformProcess::ExecProcess(
		*Exe,
		*Args,
		&Result.ReturnCode,
		&Result.Output,
		&Result.Error,
		*WorkingDir);

	Result.bSuccess = bLaunched && Result.ReturnCode == 0;

	if (!bLaunched)
	{
		UE_LOG(LogLoreQuickCommit, Error,
			TEXT("Failed to launch lore. Is '%s' installed and on PATH?"), *Exe);
	}
	else
	{
		UE_LOG(LogLoreQuickCommit, Log,
			TEXT("lore exited with code %d"), Result.ReturnCode);
		if (!Result.Output.IsEmpty())
		{
			UE_LOG(LogLoreQuickCommit, Verbose, TEXT("stdout: %s"), *Result.Output);
		}
		if (!Result.Error.IsEmpty())
		{
			UE_LOG(LogLoreQuickCommit, Warning, TEXT("stderr: %s"), *Result.Error);
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------------------
// Status queries
// ---------------------------------------------------------------------------------------

bool FLoreClient::IsRepoInitialized()
{
	// `lore status` succeeds (exit 0) only when the working directory is inside a lore
	// repository. Outside a repo it prints "Repository not found" and exits non-zero.
	// --revision-only keeps it fast by skipping the dirty-file diff.
	const FLoreResult Status = Run(TEXT("status --revision-only"));
	return Status.bSuccess;
}

bool FLoreClient::HasLocalChanges()
{
	// `lore status --scan` walks the project tree to detect added / modified / deleted
	// files. This stays cheap because .loreignore excludes the heavy generated folders
	// (Intermediate/Saved/DerivedDataCache/Binaries). We must scan here: plain `lore status`
	// only reports files lore was already told are dirty, so a freshly-saved change would
	// never light up the YELLOW state.
	const FLoreResult Status = Run(TEXT("status --scan"));
	if (!Status.bSuccess)
	{
		return false;
	}

	// These section headers appear only when there is something to report:
	//   "Untracked files:"               -> new / added files
	//   "Changes not staged for commit:" -> modified or deleted tracked files
	//   "Changes to be committed:"       -> already-staged changes
	const FString Lower = Status.Output.ToLower();
	return Lower.Contains(TEXT("untracked files")) ||
		   Lower.Contains(TEXT("not staged for commit")) ||
		   Lower.Contains(TEXT("to be committed"));
}

bool FLoreClient::IsBehindRemote(FString& OutSummary)
{
	// `lore status` already reports BOTH the local and the remote branch revision (lore is
	// online by default — there is no separate "fetch"). We are "behind" when the remote
	// revision number is higher than the local one.
	const FLoreResult Status = Run(TEXT("status"));
	OutSummary = Status.CombinedOutput();
	if (!Status.bSuccess)
	{
		return false;
	}

	const int64 LocalRevision  = ExtractIntAfter(Status.Output, TEXT("revision "));
	const int64 RemoteRevision = ExtractIntAfter(Status.Output, TEXT("Remote revision "));
	if (LocalRevision >= 0 && RemoteRevision >= 0 && RemoteRevision > LocalRevision)
	{
		return true;
	}

	// Fallback to an explicit textual hint in case the output format changes.
	return Status.Output.ToLower().Contains(TEXT("behind"));
}

// ---------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------

FString FLoreClient::SanitizeMessage(const FString& Message)
{
	// Wrap in double quotes for the shell; collapse embedded double quotes to single
	// quotes so the argument stays well-formed.
	FString Clean = Message;
	Clean.ReplaceInline(TEXT("\""), TEXT("'"));
	Clean.ReplaceInline(TEXT("\r"), TEXT(" "));
	Clean.ReplaceInline(TEXT("\n"), TEXT(" "));
	if (Clean.IsEmpty())
	{
		Clean = TEXT("Update");
	}
	return Clean;
}

bool FLoreClient::EnsureLoreIgnore()
{
	const FString IgnorePath = GetWorkingDirectory() / TEXT(".loreignore");
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (PlatformFile.FileExists(*IgnorePath))
	{
		// Respect an existing file (it may be user-customised) — never overwrite it.
		UE_LOG(LogLoreQuickCommit, Log, TEXT(".loreignore already present at %s"), *IgnorePath);
		return true;
	}

	// Default ignore list for an Unreal project. `.loreignore` uses .gitignore-style rules;
	// edit this block to change what newly-initialised projects exclude from version control.
	const FString DefaultIgnore =
		TEXT("# Unreal Engine generated / temporary folders\n")
		TEXT("Intermediate/\n")
		TEXT("Saved/\n")
		TEXT("DerivedDataCache/\n")
		TEXT("Binaries/\n")
		TEXT("Build/\n")
		TEXT("\n")
		TEXT("# IDE / editor metadata\n")
		TEXT(".vs/\n")
		TEXT(".idea/\n")
		TEXT("\n")
		TEXT("# Per-user editor settings (personal UI state, not shared between developers)\n")
		TEXT("Config/DefaultEditorPerProjectUserSettings.ini\n")
		TEXT("\n")
		TEXT("# Logs & caches\n")
		TEXT("*.log\n")
		TEXT("*.cache\n");

	if (FFileHelper::SaveStringToFile(DefaultIgnore, *IgnorePath))
	{
		UE_LOG(LogLoreQuickCommit, Log, TEXT("Wrote default .loreignore to %s"), *IgnorePath);
		return true;
	}

	UE_LOG(LogLoreQuickCommit, Warning, TEXT("Failed to write .loreignore to %s"), *IgnorePath);
	return false;
}

FLoreResult FLoreClient::InitializeProject(const FString& Host)
{
	const FString RemoteUrl = BuildRemoteUrl(Host);
	const FString ProjectDir = GetWorkingDirectory();
	UE_LOG(LogLoreQuickCommit, Log, TEXT("Initializing Lore project '%s' rooted at %s"), *RemoteUrl, *ProjectDir);

	// 0) Make sure UE generated folders are ignored BEFORE the first scan, so they never
	//    get staged into the initial commit. Created only if not already present.
	EnsureLoreIgnore();

	// 1) Create the repository on the server (named after the project) and root the LOCAL
	//    repo at the Unreal project directory. We pass --repository explicitly so the repo
	//    is rooted at the project folder and never walks up into a parent directory.
	//    Skipped if this folder is already a lore repository.
	if (!IsRepoInitialized())
	{
		Run(FString::Printf(TEXT("repository create %s --repository \"%s\""), *RemoteUrl, *ProjectDir));
	}

	// 2) Stage everything under the project. `--scan` walks the filesystem to find new /
	//    modified / deleted files and stages them in one pass (a bare `stage .` only stages
	//    files already marked dirty and would stage nothing on a fresh repo).
	Run(TEXT("stage . --scan"));

	// 3) Initial commit. In lore the message is a positional argument.
	const FLoreResult Commit = Run(TEXT("commit \"Initial Commit\""));
	if (!Commit.bSuccess)
	{
		return Commit; // e.g. nothing to commit — surface it instead of pushing.
	}

	// 4) Publish to the server.
	return Run(TEXT("push"));
}

FLoreResult FLoreClient::CommitAndPush(const FString& Message)
{
	const FString Clean = SanitizeMessage(Message);

	// Scan + stage the current project, then commit and push.
	Run(TEXT("stage . --scan"));

	const FLoreResult Commit = Run(FString::Printf(TEXT("commit \"%s\""), *Clean));
	if (!Commit.bSuccess)
	{
		return Commit; // nothing staged / commit error — don't push a non-existent commit.
	}

	return Run(TEXT("push"));
}

FLoreResult FLoreClient::PullLatest()
{
	// Destructive: discard local modifications and bring the working files in line with the
	// latest revision from the server. `--reset` resets locally modified files to match the
	// incoming revision.
	return Run(TEXT("sync --reset"));
}
