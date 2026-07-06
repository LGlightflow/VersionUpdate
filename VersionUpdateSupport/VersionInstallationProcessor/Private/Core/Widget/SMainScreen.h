
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SMainScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMainScreen)
	{}

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

protected:
	FText GetInstallationProgressTextTip() const;
	TOptional<float> GetInstallationProgressPercent() const;
private:
};