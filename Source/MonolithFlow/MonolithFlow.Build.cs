using UnrealBuildTool;
using System.IO;

public class MonolithFlow : ModuleRules
{
	public MonolithFlow(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasFlow = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Project Plugins/ folder
			string ProjectPluginsDir = Path.Combine(
				Target.ProjectFile.Directory.FullName, "Plugins");
			if (Directory.Exists(ProjectPluginsDir))
			{
				bHasFlow = Directory.Exists(Path.Combine(ProjectPluginsDir, "Flow"))
					|| Directory.GetDirectories(ProjectPluginsDir, "Flow*",
						SearchOption.TopDirectoryOnly).Length > 0;
			}

			// 2. Engine Plugins/Marketplace/ (if Flow ever ships via Fab)
			if (!bHasFlow)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
				if (Directory.Exists(MarketplaceDir))
				{
					bHasFlow = Directory.GetDirectories(MarketplaceDir, "Flow*",
						SearchOption.TopDirectoryOnly).Length > 0;
				}

				// 3. Engine Plugins/ root (manual engine-side install)
				if (!bHasFlow)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasFlow = Directory.Exists(Path.Combine(EnginePluginsDir, "Flow"));
				}
			}
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		if (bHasFlow)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore",
				"MonolithIndex",
				"SQLiteCore",
				"UnrealEd",
				"AssetRegistry",
				"BlueprintGraph",
				"Kismet",
				"EditorSubsystem",
				"Flow",
				"FlowEditor",
				"GameplayTags",
				"Json",
				"JsonUtilities"
			});
			PublicDefinitions.Add("WITH_FLOW=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MonolithCore"
			});
			PublicDefinitions.Add("WITH_FLOW=0");
		}
	}
}
