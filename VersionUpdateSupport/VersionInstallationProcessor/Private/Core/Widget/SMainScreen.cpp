#include "SMainScreen.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "VersionInstallationProcessorType.h"
#include "Core/Style/VersionInstallationProcessorStyle.h"

#define LOCTEXT_NAMESPACE "SMainScreen"

void SMainScreen::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SImage)
			.Image(FVersionInstallationProcessorStyle::Get().GetBrush("VersionInstallationProcessorStyle.Background"))
		]

		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Bottom)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Bottom)
			[
				SNew(STextBlock)
				.Text(this, &SMainScreen::GetInstallationProgressTextTip)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 24))
				.ShadowOffset(FVector2D(1.0f, 1.0f))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Bottom)
			.Padding(10.0f, 0.f,10.0f,40.f)
			[
				SNew(SProgressBar)
				.Percent(this, &SMainScreen::GetInstallationProgressPercent)
			]
		]
	];
}

FText SMainScreen::GetInstallationProgressTextTip() const
{
	return FText::Format(LOCTEXT("GetInstallationProgressTextTip","InstallationProgress : {0} % "),(int32)(ProgressInstallationPercentage * 100.f));
}

TOptional<float> SMainScreen::GetInstallationProgressPercent() const
{
	return ProgressInstallationPercentage;
}

#undef LOCTEXT_NAMESPACE