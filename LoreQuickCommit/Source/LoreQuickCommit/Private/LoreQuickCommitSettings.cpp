// Copyright Epic Games, Inc. All Rights Reserved.

#include "LoreQuickCommitSettings.h"

ULoreQuickCommitSettings::ULoreQuickCommitSettings()
{
}

ULoreQuickCommitSettings* ULoreQuickCommitSettings::GetMutable()
{
	return GetMutableDefault<ULoreQuickCommitSettings>();
}

const ULoreQuickCommitSettings* ULoreQuickCommitSettings::Get()
{
	return GetDefault<ULoreQuickCommitSettings>();
}

void ULoreQuickCommitSettings::SaveToConfig()
{
	// Write to the PER-USER config layer (Saved/Config/.../EditorPerProjectUserSettings.ini)
	// rather than the committed Default config. This keeps each developer's Lore server
	// settings on their own machine and out of version control. (Using SaveConfig() instead
	// of TryUpdateDefaultConfigFile() is what targets the user layer.)
	SaveConfig();
}
