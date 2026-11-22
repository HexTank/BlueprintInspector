// Copyright (c) 2024-2026 Paul Tankard

using System.IO;
#if UE_5_0_OR_LATER
using EpicGames.Core;
#else
using Tools.DotNETCommon;
#endif
using UnrealBuildTool;

public class BlueprintInspector : ModuleRules
{
	public BlueprintInspector(ReadOnlyTargetRules target) : base(target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		bEnableExceptions = true;

		if (target.Version.MajorVersion == 4 && target.Version.MinorVersion < 25)
		{
			throw new BuildException(string.Format(
				"BlueprintInspector supports UE 4.25 and later; {0}.{1} predates FProperty/FField.",
				target.Version.MajorVersion, target.Version.MinorVersion));
		}

		bool bVerifiedEngine = (target.Version.MajorVersion == 4 && target.Version.MinorVersion >= 25)
		                       || (target.Version.MajorVersion == 5 && target.Version.MinorVersion <= 8);
		if (!bVerifiedEngine)
		{
			Log.TraceWarning(string.Format(
				"BlueprintInspector: UE {0}.{1} is outside the verified matrix (4.25-5.8); the module may build, " +
				"but the automation goldens and token plans are only recorded for those engines.",
				target.Version.MajorVersion, target.Version.MinorVersion));
		}

		// UE 5.6 and later require C++20 in their own headers, and 5.8 no longer allows C++17, so let
		// those engines pick their default standard. Older engines keep the pin: Latest maps to
		// /std:c++latest, which enables preview rules and breaks the 4.25 engine headers.
		if (target.Version.MajorVersion == 4 || (target.Version.MajorVersion == 5 && target.Version.MinorVersion < 6))
		{
#pragma warning disable 618
			CppStandard = CppStandardVersion.Cpp17;
#pragma warning restore 618
		}

		if (target.Platform == UnrealTargetPlatform.Win64)
		{
			bUseRTTI = true;
			// The include paths for both libraries are private, so their compile definitions stay private
			// too. UBT has no private link list (PublicAdditionalLibraries is the only one), so the link
			// itself is necessarily visible to dependents; no public header exposes these libs.
			PrivateDefinitions.Add("DETOUR_DEBUG=0");
			PrivateDefinitions.Add("CPPDAP_JSON_NLOHMANN=1");

			string ThirdPartyPath = Path.Combine(ModuleDirectory, "..", "ThirdParty");

			// One prebuilt pair per MSVC line the supported engines use. The libraries are maintained
			// in the repository, so the module needs no CMake or nmake step.
			string ToolsetLine = target.Version.MajorVersion >= 5 ? "v143" : "v142";
			string CompilerVersion = target.WindowsPlatform.CompilerVersion;
			if (!string.IsNullOrEmpty(CompilerVersion))
			{
				string[] Parts = CompilerVersion.Split('.');
				int CompilerMajor = 0;
				int CompilerMinor = 0;
				if (Parts.Length >= 2 && int.TryParse(Parts[0], out CompilerMajor) && CompilerMajor == 14 && int.TryParse(Parts[1], out CompilerMinor))
				{
					ToolsetLine = CompilerMinor >= 30 ? "v143" : "v142";
				}
			}
			string PrebuiltDir = Path.Combine(ThirdPartyPath, "lib", ToolsetLine);

			string VendoredInclude = Path.Combine(ThirdPartyPath, "include");
			PrivateIncludePaths.Add(Path.Combine(VendoredInclude, "cppdap"));
			string CppdapLib = Path.Combine(PrebuiltDir, "cppdap.lib");
			if (!File.Exists(CppdapLib))
			{
				throw new BuildException(string.Format(
					"BlueprintInspector: {0} is missing. The prebuilt libraries are tracked per MSVC line.", CppdapLib));
			}
			PublicAdditionalLibraries.Add(CppdapLib);

			PrivateIncludePaths.Add(Path.Combine(VendoredInclude, "detours"));
			string DetoursLib = Path.Combine(PrebuiltDir, "detours.lib");
			if (!File.Exists(DetoursLib))
			{
				throw new BuildException(string.Format(
					"BlueprintInspector: {0} is missing. The prebuilt libraries are tracked per MSVC line.", DetoursLib));
			}
			PublicAdditionalLibraries.Add(DetoursLib);
		}

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				// OpcodeProbeHost.h is public and derives from UObject, so its module must be public too.
				"Core",
				"CoreUObject"
			}
		);

		// The pause yields the game viewport's pointer capture through FSlateApplication and takes it back
		// through the viewport's focus path. Engine depends on Slate publicly, but UBT does not link that
		// transitive import library into this module, so Slate (and the SlateCore types it hands around)
		// has to be declared here or the symbols stay unresolved.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"Slate",
				"SlateCore"
			}
		);

		// The fixture graph generator archives function graphs as text and rebuilds them with the
		// editor's K2 API. It never ships, so keep the editor modules behind bBuildEditor; the test
		// sources themselves are gated on WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR.
		if (target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"UnrealEd",
					"BlueprintGraph",
					// The tests resolve the plugin base directory to find Data/BlueprintLayoutGuide.md.
					"Projects"
				}
			);
		}

	}
}