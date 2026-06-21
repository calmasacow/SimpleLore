// Copyright Epic Games, Inc. All Rights Reserved.

#include "LoreQuickCommitModule.h"
#include "LoreQuickCommitSettings.h"
#include "LoreClient.h"

#include "Modules/ModuleManager.h"

// Toolbar
#include "ToolMenus.h"
#include "Textures/SlateIcon.h"
#include "Styling/AppStyle.h"

// Slate widgets
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Brushes/SlateRoundedBoxBrush.h"

// Editor + engine services
#include "FileHelpers.h"               // FEditorFileUtils
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Async/Async.h"
#include "UObject/Package.h"           // UPackage::PackageSavedWithContextEvent
#include "UObject/ObjectSaveContext.h" // FObjectPostSaveContext

// HTTP health check
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#define LOCTEXT_NAMESPACE "FLoreQuickCommitModule"

namespace
{
	/** A 14x14 fully-rounded (circle) brush, tinted at draw time via ColorAndOpacity. */
	const FSlateBrush* GetStatusCircleBrush()
	{
		static const FSlateRoundedBoxBrush CircleBrush(FLinearColor::White, 7.0f, FVector2f(14.0f, 14.0f));
		return &CircleBrush;
	}

	/** Best-effort parent window for modal dialogs. */
	TSharedPtr<SWindow> GetParentWindow()
	{
		return FSlateApplication::Get().GetActiveTopLevelWindow();
	}
}

// =======================================================================================
// Module lifecycle
// =======================================================================================

void FLoreQuickCommitModule::StartupModule()
{
	UE_LOG(LogLoreQuickCommit, Log, TEXT("LoreQuickCommit starting up."));

	// Register the toolbar once UToolMenus is ready (it may not be at this point).
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FLoreQuickCommitModule::RegisterToolbar));

	// Start the periodic poll that watches for newer commits on the server.
	const ULoreQuickCommitSettings* Settings = ULoreQuickCommitSettings::Get();
	const float Interval = Settings ? FMath::Clamp(Settings->PollIntervalSeconds, 30.0f, 120.0f) : 45.0f;
	PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FLoreQuickCommitModule::OnPollTimer), Interval);

	// Refresh immediately whenever the user saves an asset or level, so the indicator turns
	// YELLOW right away instead of waiting for the next poll tick.
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(
		this, &FLoreQuickCommitModule::OnPackageSaved);
}

void FLoreQuickCommitModule::ShutdownModule()
{
	UE_LOG(LogLoreQuickCommit, Log, TEXT("LoreQuickCommit shutting down."));

	if (PollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
		PollTickerHandle.Reset();
	}

	if (PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
		PackageSavedHandle.Reset();
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (TSharedPtr<SWindow> Window = SettingsWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
}

FLoreQuickCommitModule& FLoreQuickCommitModule::Get()
{
	return FModuleManager::LoadModuleChecked<FLoreQuickCommitModule>("LoreQuickCommit");
}

bool FLoreQuickCommitModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("LoreQuickCommit");
}

// =======================================================================================
// Toolbar
// =======================================================================================

void FLoreQuickCommitModule::RegisterToolbar()
{
	if (!UToolMenus::IsToolMenuUIEnabled())
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(this);

	// Extend the user/right-hand section of the main Level Editor toolbar.
	UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
	if (!Toolbar)
	{
		UE_LOG(LogLoreQuickCommit, Warning, TEXT("Could not find the Level Editor toolbar to extend."));
		return;
	}

	FToolMenuSection& Section = Toolbar->FindOrAddSection("Lore");

	// 1) Status indicator (colored circle + "LORE STATUS:" label).
	Section.AddEntry(FToolMenuEntry::InitWidget(
		"LoreStatusIndicator",
		BuildStatusIndicatorWidget(),
		FText::GetEmpty(),
		/*bNoIndent*/ true,
		/*bSearchable*/ false));

	// 2) Settings (gear) button.
	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"LoreSettings",
		FUIAction(FExecuteAction::CreateRaw(this, &FLoreQuickCommitModule::OnSettingsButtonClicked)),
		LOCTEXT("LoreSettingsLabel", "Lore"),
		LOCTEXT("LoreSettingsTooltip", "Open the Lore settings panel (connect / initialize)."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings")));

	// 3) Main action button (commit / sync / resolve).
	Section.AddEntry(FToolMenuEntry::InitToolBarButton(
		"LoreAction",
		FUIAction(FExecuteAction::CreateRaw(this, &FLoreQuickCommitModule::OnActionButtonClicked)),
		LOCTEXT("LoreActionLabel", "Commit"),
		LOCTEXT("LoreActionTooltip", "Commit & push local changes, or resolve remote changes."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Save")));

	// Do a first status refresh now that the editor is up.
	RefreshStatusAsync();
}

TSharedRef<SWidget> FLoreQuickCommitModule::BuildStatusIndicatorWidget()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 4.0f, 0.0f)
		[
			SNew(SImage)
			.Image(GetStatusCircleBrush())
			.ColorAndOpacity_Lambda([this]() { return FSlateColor(GetStatusColor()); })
			.ToolTipText_Lambda([this]()
			{
				return FText::Format(LOCTEXT("StatusTooltip", "Lore status: {0}"), GetStatusText());
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("LoreStatusLabel", "LORE STATUS:"))
		];
}

void FLoreQuickCommitModule::OnSettingsButtonClicked()
{
	OpenSettingsPanel();
}

void FLoreQuickCommitModule::OnActionButtonClicked()
{
	const ELoreStatus Status = CurrentStatus;

	// RED: nothing useful to commit until the user connects & initializes.
	if (Status == ELoreStatus::Disconnected)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			LOCTEXT("NotReady",
				"Lore is not connected or the project is not initialized.\n\n"
				"Open the Lore settings (gear icon) to connect and initialize."));
		OpenSettingsPanel();
		return;
	}

	// All action paths require a saved project first.
	if (HasUnsavedChanges())
	{
		if (!PromptSaveChanges())
		{
			// User cancelled the save dialog — abort the action.
			return;
		}
	}

	if (Status == ELoreStatus::Behind)
	{
		// Gather a fresh summary of what the server has that we don't, then show it.
		FString Summary;
		{
			FScopedSlowTask SlowTask(0.0f, LOCTEXT("CheckingRemote", "Checking Lore server for changes..."));
			SlowTask.MakeDialog();
			FLoreClient::IsBehindRemote(Summary);
		}
		OpenBehindPanel(Summary);
	}
	else
	{
		// GREEN or YELLOW: straightforward commit & push.
		OpenCommitPanel();
	}
}

// =======================================================================================
// Status helpers
// =======================================================================================

FLinearColor FLoreQuickCommitModule::GetStatusColor() const
{
	switch (CurrentStatus)
	{
	case ELoreStatus::Synced:       return FLinearColor::Green;
	case ELoreStatus::LocalChanges: return FLinearColor::Yellow;
	// Orange #FF6600. Built from an sRGB hex via FColor so the on-screen color matches the
	// hex (Slate renders in linear space; the FColor->FLinearColor ctor does the gamma).
	case ELoreStatus::Behind:       return FLinearColor(FColor::FromHex(TEXT("FF6600")));
	case ELoreStatus::Disconnected:
	default:                        return FLinearColor::Red;
	}
}

FText FLoreQuickCommitModule::GetStatusText() const
{
	switch (CurrentStatus)
	{
	case ELoreStatus::Synced:       return LOCTEXT("StatusSynced", "In Sync");
	case ELoreStatus::LocalChanges: return LOCTEXT("StatusLocal", "Local Changes");
	case ELoreStatus::Behind:       return LOCTEXT("StatusBehind", "Behind Server");
	case ELoreStatus::Disconnected:
	default:                        return LOCTEXT("StatusDisconnected", "Disconnected / Not Initialized");
	}
}

void FLoreQuickCommitModule::SetStatus(ELoreStatus NewStatus)
{
	if (CurrentStatus != NewStatus)
	{
		CurrentStatus = NewStatus;
		UE_LOG(LogLoreQuickCommit, Log, TEXT("Status changed -> %s"), *GetStatusText().ToString());
	}
}

bool FLoreQuickCommitModule::OnPollTimer(float /*DeltaTime*/)
{
	RefreshStatusAsync();
	return true; // keep ticking
}

void FLoreQuickCommitModule::OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext SaveContext)
{
	// Only react to genuine user-driven saves. Skip procedural saves (cooking, PIE
	// duplication, etc.) which aren't real content edits the user is committing.
	if (Package == nullptr || SaveContext.IsProceduralSave())
	{
		return;
	}

	UE_LOG(LogLoreQuickCommit, Verbose, TEXT("Package saved (%s) -> refreshing Lore status."), *PackageFilename);

	// RefreshStatusAsync runs the scan on a worker thread; the in-flight guard collapses
	// the burst of save events from a multi-package save into a single refresh.
	RefreshStatusAsync();
}

void FLoreQuickCommitModule::RefreshStatusAsync()
{
	if (bRefreshInFlight)
	{
		return;
	}

	const ULoreQuickCommitSettings* Settings = ULoreQuickCommitSettings::Get();
	if (!Settings || !Settings->bEnabled || Settings->ServerHost.IsEmpty())
	{
		bServerReachable = false;
		bRepoInitialized = false;
		SetStatus(ELoreStatus::Disconnected);
		return;
	}

	bRefreshInFlight = true;
	const FString HealthUrl = FLoreClient::BuildHealthCheckUrl(Settings->ServerHost);

	// Step 1: async HTTP health check (non-blocking, completes on the game thread).
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(HealthUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(5.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			const bool bHealthy = bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() == 200;
			bServerReachable = bHealthy;

			if (!bHealthy)
			{
				bRefreshInFlight = false;
				SetStatus(ELoreStatus::Disconnected);
				return;
			}

			// Step 2: server is reachable. Run the (blocking) lore queries on a worker
			// thread so the editor doesn't hitch, then apply the result on the game thread.
			Async(EAsyncExecution::Thread, [this]()
			{
				const bool bInitialized = FLoreClient::IsRepoInitialized();
				bool bBehind = false;
				bool bLocal = false;
				if (bInitialized)
				{
					FString IgnoredSummary;
					bBehind = FLoreClient::IsBehindRemote(IgnoredSummary);
					bLocal = FLoreClient::HasLocalChanges();
				}

				AsyncTask(ENamedThreads::GameThread, [this, bInitialized, bBehind, bLocal]()
				{
					bRepoInitialized = bInitialized;

					if (!bInitialized)
					{
						SetStatus(ELoreStatus::Disconnected); // reachable but not initialized => RED
					}
					else if (bBehind)
					{
						SetStatus(ELoreStatus::Behind);       // ORANGE
					}
					else if (bLocal)
					{
						SetStatus(ELoreStatus::LocalChanges); // YELLOW
					}
					else
					{
						SetStatus(ELoreStatus::Synced);       // GREEN
					}

					bRefreshInFlight = false;
				});
			});
		});

	Request->ProcessRequest();
}

// =======================================================================================
// Save helpers
// =======================================================================================

bool FLoreQuickCommitModule::HasUnsavedChanges()
{
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
	FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
	return DirtyPackages.Num() > 0;
}

bool FLoreQuickCommitModule::PromptSaveChanges()
{
	// Shows the standard "Save Content" dialog. Returns false only if the user cancels.
	const bool bNotCancelled = FEditorFileUtils::SaveDirtyPackages(
		/*bPromptUserToSave*/ true,
		/*bSaveMapPackages*/ true,
		/*bSaveContentPackages*/ true,
		/*bFastSave*/ false,
		/*bNotifyNoPackagesSaved*/ false,
		/*bCanBeDeclined*/ true);
	return bNotCancelled;
}

// =======================================================================================
// Settings panel
// =======================================================================================

void FLoreQuickCommitModule::OpenSettingsPanel()
{
	// If it's already open, just bring it forward.
	if (TSharedPtr<SWindow> Existing = SettingsWindow.Pin())
	{
		Existing->BringToFront();
		return;
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("SettingsTitle", "Lore Settings"))
		.ClientSize(FVector2D(420.0f, 240.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			// Enabled / disabled line.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					const ULoreQuickCommitSettings* S = ULoreQuickCommitSettings::Get();
					return FText::Format(LOCTEXT("EnabledLine", "Lore version control: {0}"),
						(S && S->bEnabled) ? LOCTEXT("Enabled", "Enabled") : LOCTEXT("Disabled", "Disabled"));
				})
			]

			// Server host line.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					const ULoreQuickCommitSettings* S = ULoreQuickCommitSettings::Get();
					const FString Host = (S && !S->ServerHost.IsEmpty()) ? S->ServerHost : TEXT("(none)");
					return FText::Format(LOCTEXT("ServerLine", "Server: {0}"), FText::FromString(Host));
				})
			]

			// Connection status line (colored).
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.ColorAndOpacity_Lambda([this]()
				{
					return FSlateColor(bServerReachable ? FLinearColor::Green : FLinearColor::Red);
				})
				.Text_Lambda([this]()
				{
					return bServerReachable
						? LOCTEXT("Connected", "Connection: Connected")
						: LOCTEXT("NotConnected", "Connection: Not connected");
				})
			]

			// Connect button — only when not reachable.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Visibility_Lambda([this]()
				{
					return bServerReachable ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text(LOCTEXT("ConnectBtn", "Connect..."))
				.OnClicked_Lambda([this]()
				{
					FString Host;
					if (PromptForServerHost(Host))
					{
						ULoreQuickCommitSettings* S = ULoreQuickCommitSettings::GetMutable();
						S->ServerHost = Host;
						S->SaveToConfig();
						UE_LOG(LogLoreQuickCommit, Log, TEXT("Server host set to '%s'. Probing..."), *Host);
						// Health check will flip bServerReachable; the panel updates itself.
						RefreshStatusAsync();
					}
					return FReply::Handled();
				})
			]

			// Initialize button — only when connected but not yet initialized.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Visibility_Lambda([this]()
				{
					return (bServerReachable && !bRepoInitialized) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(LOCTEXT("InitBtn", "Initialize Project"))
				.OnClicked_Lambda([this]()
				{
					const ULoreQuickCommitSettings* S = ULoreQuickCommitSettings::Get();
					if (!S || S->ServerHost.IsEmpty())
					{
						return FReply::Handled();
					}

					FLoreResult Result;
					{
						FScopedSlowTask SlowTask(0.0f,
							LOCTEXT("Initializing", "Creating repository and pushing initial commit..."));
						SlowTask.MakeDialog();
						Result = FLoreClient::InitializeProject(S->ServerHost);
					}

					if (Result.bSuccess)
					{
						ULoreQuickCommitSettings* Mutable = ULoreQuickCommitSettings::GetMutable();
						Mutable->bInitialized = true;
						Mutable->SaveToConfig();
						bRepoInitialized = true;
						SetStatus(ELoreStatus::Synced); // GREEN
						RefreshStatusAsync();
					}
					else
					{
						FMessageDialog::Open(EAppMsgType::Ok,
							FText::Format(LOCTEXT("InitFailed", "Initialize failed:\n\n{0}"),
								FText::FromString(Result.CombinedOutput())));
					}
					return FReply::Handled();
				})
			]

			// "Connected and Initialized" confirmation.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(FLinearColor::Green))
				.Visibility_Lambda([this]()
				{
					return (bServerReachable && bRepoInitialized) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(LOCTEXT("AllGood", "Connected and Initialized"))
			]

			// Spacer + Close button at the bottom.
			+ SVerticalBox::Slot().FillHeight(1.0f)[ SNullWidget::NullWidget ]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SButton)
				.Text(LOCTEXT("CloseBtn", "Close"))
				.OnClicked_Lambda([this]()
				{
					if (TSharedPtr<SWindow> W = SettingsWindow.Pin())
					{
						W->RequestDestroyWindow();
					}
					return FReply::Handled();
				})
			]
		]
	);

	SettingsWindow = Window;
	Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([this](const TSharedRef<SWindow>&)
	{
		SettingsWindow.Reset();
	}));

	FSlateApplication::Get().AddWindow(Window);
}

// =======================================================================================
// Server host prompt (modal)
// =======================================================================================

bool FLoreQuickCommitModule::PromptForServerHost(FString& OutHost)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("ConnectTitle", "Connect to Lore Server"))
		.ClientSize(FVector2D(380.0f, 140.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SEditableTextBox> HostBox;
	bool bAccepted = false;

	const FString Current = ULoreQuickCommitSettings::Get()->ServerHost;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).Text(LOCTEXT("HostPrompt", "Enter the Lore server IP or hostname:"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SAssignNew(HostBox, SEditableTextBox)
				.Text(FText::FromString(Current))
				.HintText(LOCTEXT("HostHint", "e.g. 192.168.1.200"))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ConnectConfirm", "Connect"))
					.OnClicked_Lambda([&OutHost, &bAccepted, HostBox, Window]()
					{
						const FString Entered = HostBox->GetText().ToString().TrimStartAndEnd();
						if (!Entered.IsEmpty())
						{
							OutHost = Entered;
							bAccepted = true;
							FSlateApplication::Get().RequestDestroyWindow(Window);
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("ConnectCancel", "Cancel"))
					.OnClicked_Lambda([Window]()
					{
						FSlateApplication::Get().RequestDestroyWindow(Window);
						return FReply::Handled();
					})
				]
			]
		]
	);

	// Blocks until the window is destroyed; OutHost / bAccepted are filled by the buttons.
	FSlateApplication::Get().AddModalWindow(Window, GetParentWindow());
	return bAccepted;
}

// =======================================================================================
// Commit panel (GREEN / YELLOW)
// =======================================================================================

void FLoreQuickCommitModule::OpenCommitPanel()
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("CommitTitle", "Commit to Lore"))
		.ClientSize(FVector2D(440.0f, 240.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SMultiLineEditableTextBox> MessageBox;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock).Text(LOCTEXT("CommitPrompt", "Commit message:"))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 0, 0, 12)
			[
				SAssignNew(MessageBox, SMultiLineEditableTextBox)
				.HintText(LOCTEXT("CommitHint", "Describe your changes..."))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ContentPadding(FMargin(0, 10))
				.Text(LOCTEXT("SendBtn", "SEND"))
				.OnClicked_Lambda([this, MessageBox, Window]()
				{
					const FString Message = MessageBox->GetText().ToString().TrimStartAndEnd();

					FLoreResult Result;
					{
						FScopedSlowTask SlowTask(0.0f, LOCTEXT("Committing", "Staging, committing and pushing to Lore..."));
						SlowTask.MakeDialog();
						Result = FLoreClient::CommitAndPush(Message);
					}

					if (Result.bSuccess)
					{
						FSlateApplication::Get().RequestDestroyWindow(Window);
						SetStatus(ELoreStatus::Synced);
						RefreshStatusAsync();
					}
					else
					{
						FMessageDialog::Open(EAppMsgType::Ok,
							FText::Format(LOCTEXT("CommitFailed", "Commit/push failed:\n\n{0}"),
								FText::FromString(Result.CombinedOutput())));
					}
					return FReply::Handled();
				})
			]
		]
	);

	FSlateApplication::Get().AddWindow(Window);
}

// =======================================================================================
// Behind panel (ORANGE) — destructive pull
// =======================================================================================

void FLoreQuickCommitModule::OpenBehindPanel(const FString& DiffSummary)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("BehindTitle", "Lore: Behind Server"))
		.ClientSize(FVector2D(520.0f, 360.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	const FString SummaryText = DiffSummary.IsEmpty()
		? TEXT("The Lore server has newer commits than your local project.")
		: DiffSummary;

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(14.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("BehindHeader", "Differences between your project and the Lore server:"))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 0, 0, 12)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(8.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(STextBlock)
						.Text(FText::FromString(SummaryText))
						.AutoWrapText(true)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("AcceptBtn", "Accept Changes"))
					.OnClicked_Lambda([this, Window]()
					{
						// Clear, explicit destructive-action warning + final Yes/No.
						const EAppReturnType::Type Choice = FMessageDialog::Open(
							EAppMsgType::YesNo,
							LOCTEXT("DestructiveWarning",
								"This is a destructive action.\n\n"
								"Your local files will be OVERWRITTEN by the version from the Lore server. "
								"Any local changes that have not been committed and pushed will be lost.\n\n"
								"Are you sure you want to pull and overwrite local files?"));

						if (Choice == EAppReturnType::Yes)
						{
							FLoreResult Result;
							{
								FScopedSlowTask SlowTask(0.0f, LOCTEXT("Pulling", "Pulling latest from Lore server..."));
								SlowTask.MakeDialog();
								Result = FLoreClient::PullLatest();
							}

							FSlateApplication::Get().RequestDestroyWindow(Window);

							if (Result.bSuccess)
							{
								SetStatus(ELoreStatus::Synced);
								RefreshStatusAsync();
							}
							else
							{
								FMessageDialog::Open(EAppMsgType::Ok,
									FText::Format(LOCTEXT("PullFailed", "Pull failed:\n\n{0}"),
										FText::FromString(Result.CombinedOutput())));
							}
						}
						// If "No": leave status as Orange and just close this panel below.
						else
						{
							FSlateApplication::Get().RequestDestroyWindow(Window);
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CloseBehindBtn", "Close"))
					.OnClicked_Lambda([Window]()
					{
						FSlateApplication::Get().RequestDestroyWindow(Window);
						return FReply::Handled();
					})
				]
			]
		]
	);

	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLoreQuickCommitModule, LoreQuickCommit)
