#include "MonolithFlowModule.h"
#include "MonolithFlowActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"

#if WITH_FLOW
#include "MonolithFlowIndexer.h"
#include "MonolithIndexSubsystem.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#endif

#define LOCTEXT_NAMESPACE "FMonolithFlowModule"

DEFINE_LOG_CATEGORY(LogMonolithFlow);

void FMonolithFlowModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings)
	{
		return;
	}

#if WITH_FLOW
	if (Settings->bEnableFlow)
	{
		FMonolithFlowActions::RegisterActions(FMonolithToolRegistry::Get());
		const int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("flow")).Num();
		UE_LOG(LogMonolithFlow, Log,
			TEXT("MonolithFlow: Loaded (%d actions)"), ActionCount);
	}
	else
	{
		UE_LOG(LogMonolithFlow, Log,
			TEXT("MonolithFlow: actions disabled in settings (bEnableFlow=false)"));
	}

	if (Settings->bIndexFlow)
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			if (GEditor)
			{
				if (UMonolithIndexSubsystem* IndexSS = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>())
				{
					IndexSS->RegisterIndexer(MakeShared<FMonolithFlowIndexer>());
					UE_LOG(LogMonolithFlow, Log, TEXT("MonolithFlow: Registered FMonolithFlowIndexer into MonolithIndex"));
				}
			}
		});
	}
#else
	UE_LOG(LogMonolithFlow, Log,
		TEXT("MonolithFlow: Flow plugin not found at compile time, bridge inactive"));
#endif
}

void FMonolithFlowModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("flow"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithFlowModule, MonolithFlow)
