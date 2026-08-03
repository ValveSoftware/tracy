
param(
    [string]$dest,
    [Switch]$help,
    [Switch]$compile,
    [Switch]$keepGenerated,
    [Switch]$makeAll,
    [Switch]$profiler,
    [Switch]$capture,
    [Switch]$serverapi
)


$cmakeProjects = @("ALL_BUILD", "INSTALL", "PACKAGE", "ZERO_CHECK", "CMakePredefinedTargets", "ExternalProjectTargets")
$externDeps = @( "capstone", "freetype", "glfw", "rapidjson", "nfd", "zstd", "imgui", "ppqsort", "git-ref", "json", "base64", "md4c", "libcurl", "pugixml", "tidy", "embed", "usearch" )

function Cmake-GenerateProjects
{
    param(
        [string]$src,
        [string]$project
    )

    Write-Host "Generate the project files for '$project'" -ForegroundColor DarkCyan

    $cmakeCommandParts = @(
        "cmake",
        "-B $src",
        "-S $project",
        "-DCMAKE_CONFIGURATION_TYPES=`"Debug;Release`"",
        "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG=`"false`"",
        "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=`"false`"",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=`"MultiThreaded`$<`$<CONFIG:Debug>:Debug>`"",
        "-DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=`"false`"",
        "-DCMAKE_SKIP_PACKAGE_ALL_DEPENDENCY=`"false`"",
        "-DCMAKE_SUPPRESS_REGENERATION=`"true`""
    )

    $cmakeCommand = $cmakeCommandParts -join ' '
    Write-Host $cmakeCommand -ForegroundColor DarkGreen
    Write-Host ""

    Invoke-Expression $cmakeCommand | Out-Null

    $result = [bool]($LastExitCode -eq 0)
    if ( $result -eq $false ) {
        Write-Host "Cmake failed!" -ForegroundColor Red
    }

    Write-Host ""
    return [bool]$result
}


function Compile-Dependencies
{
    param(
        [string]$srcPath
    )

    $srcFullPath = (Get-Item (Convert-Path $srcPath)).FullName
    $depsPath = Join-Path -Path $srcPath -ChildPath "_deps"

    if ( Test-Path -Path "$depsPath" -PathType Container ) {
        Write-Host "Build the external dependencies" -ForegroundColor DarkCyan

        Push-Location $depsPath

        $commonBase = (Get-Item .).FullName

        # git-ref special case...
        $gitrefPrjFullPath = Join-Path -Path "$srcFullPath" -ChildPath "git-ref.vcxproj"
        if ( Test-Path -Path $gitrefPrjFullPath -PathType Leaf ) {
            Write-Host "Building: prj file: 'git-ref.vcxproj'" -ForegroundColor DarkYellow

            $msbuildCmd = 'msbuild /nologo /verbosity:quiet /p:Configuration=__config__ /p:OutDir="__outdir__" /t:Rebuild ' + $gitrefPrjFullPath

            $compRel = $msbuildCmd.Replace( '__config__', 'Release' ).Replace( '__outdir__', $outFullPath + 'Release\\' )
            Write-Host $compRel -ForegroundColor DarkGreen
            Invoke-Expression $compRel

            Write-Host ""
        }

        # embed special case...
        $embedPrjFullPath = Join-Path -Path "$srcFullPath" -ChildPath "embed.vcxproj"
        if ( Test-Path -Path $embedPrjFullPath -PathType Leaf ) {
            Write-Host "Building: prj file: 'embed.vcxproj'" -ForegroundColor DarkYellow

            $depsFullPath = Join-Path -Path $srcFullPath -ChildPath "_deps"
            $outFullPath = Join-Path -Path $depsFullPath -ChildPath "embed-build"
            New-Item -Path "$outFullPath" -ItemType Directory -Force | Out-Null

            if ( -Not $outFullPath.EndsWith( [System.IO.Path]::DirectorySeparatorChar ) ) {
                $outFullPath = $outFullPath + [System.IO.Path]::DirectorySeparatorChar
            }

            Push-Location "$outFullPath"

            $outFullPath = $outFullPath.Replace( "\", "\\" )
            $msbuildCmd = 'msbuild /nologo /verbosity:quiet /p:Configuration=__config__ /p:OutDir="__outdir__" /t:Rebuild ' + $embedPrjFullPath

            $compRel = $msbuildCmd.Replace( '__config__', 'Release' ).Replace( '__outdir__', $outFullPath + 'Release\\' )
            Write-Host $compRel -ForegroundColor DarkGreen
            Invoke-Expression $compRel

            Pop-Location

            $profPrj = Join-Path -Path "$srcFullPath" -ChildPath "tracy-profiler.vcxproj"
            if ( Test-Path -Path $profPrj -PathType Leaf ) {
                $content = Get-Content -Path $profPrj

                [XML]$xml = $content
                $nsUri = "http://schemas.microsoft.com/developer/msbuild/2003"

                $nsManager = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
                $nsManager.AddNamespace("ns", $nsUri)
                $customBuildNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:ItemGroup/ns:CustomBuild" -attr 'Include' -filters @( ".rule" )

                if (@($customBuildNodes).Count -gt 0) {
                    $embedDataDir = Join-Path -Path "$srcFullPath" -ChildPath "data"
                    New-Item -ItemType Directory -Force -Path "$embedDataDir" | Out-Null

                    Push-Location "$srcFullPath"
                    $embed = Join-Path -Path (Get-Location).Path -ChildPath "embed.exe"

                    foreach ( $node in $customBuildNodes ) {
                        if ( $node -is [System.Xml.XmlNode] ) {
                            $commandNodes = $node.SelectNodes( "ns:Command", $nsManager )

                            foreach ( $commandNode in $commandNodes ) {
                                $cond = $commandNode.GetAttribute( "Condition" )
                                if ( $cond -ne $null -and $cond -like  "*`$(Configuration)*==*Release*") {
                                    $command = (($commandNode.InnerXml -split "`r?`n") -match "embed" | Out-String).Trim()
                                    if ( $command -like "*embed*" ) {
                                        $commandargs = $command.Replace( "embed ", "" ).Trim()
                                        Start-Process -FilePath $embed -ArgumentList $commandargs -NoNewWindow -Wait | Out-Null
                                    }
                                }
                            }
                        }
                    }

                    Pop-Location
                }

                Remove-MatchingNodes -nodes $customBuildNodes
            }

            Write-Host ""
        }

        # imgui special case...
        $imguiPrjFullPath = Join-Path -Path "$srcFullPath" -ChildPath "TracyImGui.vcxproj"
        if ( Test-Path -Path $imguiPrjFullPath -PathType Leaf ) {
            Write-Host "Building: prj file: 'TracyImGui.vcxproj'" -ForegroundColor DarkYellow

            $depsFullPath = Join-Path -Path $srcFullPath -ChildPath "_deps"
            $outFullPath = Join-Path -Path $depsFullPath -ChildPath "imgui-build"
            New-Item -Path "$outFullPath" -ItemType Directory -Force | Out-Null

            if ( -Not $outFullPath.EndsWith( [System.IO.Path]::DirectorySeparatorChar ) ) {
                $outFullPath = $outFullPath + [System.IO.Path]::DirectorySeparatorChar
            }

            Push-Location "$outFullPath"

            $outFullPath = $outFullPath.Replace( "\", "\\" )
            $msbuildCmd = 'msbuild /nologo /verbosity:quiet /p:Configuration=__config__ /p:OutDir="__outdir__" /t:Rebuild ' + $imguiPrjFullPath

            $compDbg = $msbuildCmd.Replace( '__config__', 'Debug' ).Replace( '__outdir__', $outFullPath + 'Debug\\' )
            Write-Host $compDbg -ForegroundColor DarkGreen
            Invoke-Expression $compDbg

            $compRel = $msbuildCmd.Replace( '__config__', 'Release' ).Replace( '__outdir__', $outFullPath + 'Release\\' )
            Write-Host $compRel -ForegroundColor DarkGreen
            Invoke-Expression $compRel

            Pop-Location

            Write-Host ""
        }

        $alldirs = Get-ChildItem -Path "." -Directory
        $dirs = $alldirs
        $buildDirs = Get-ChildItem -Path "." -Directory | Where-Object { $_.Name.EndsWith('-build')}

        $depProjectFiles = @()

        foreach ( $bd in $buildDirs ) {
            $checkDirs = Get-ChildItem -Path $bd -Directory -Recurse

            $depProj = $bd.Name.Replace( "-build", "" )
            if ( $depProj -eq "pugixml" ) {
                $depProj += "-static"
            }

            if ( $depProj -eq "libcurl" ) {
                $depProj = "libcurl_static"
            }

            if ( $depProj -eq "capstone" ) {
                $depProj = "capstone_static"
            }

            $prjName = [io.path]::ChangeExtension( $depProj, "vcxproj" )
            $depProjFile = (Get-ChildItem -Path "$bd" -Filter "$prjName" -Recurse -File).FullName

            if ( -not [string]::IsNullOrEmpty( $depProjFile ) -and (Test-Path -Path $depProjFile) ) {
                $relPath = $depProjFile.Substring( $commonBase.Length ).TrimStart( '\' )
                $depProjectFiles += $relPath
            }
        }

        foreach ( $dep in $depProjectFiles ) {
            $relPath = (Get-Item $dep).DirectoryName
            $prjFile = (Get-Item $dep).Name

            Write-Host "Building: prj file: '$prjFile', relPath: '$relPath'" -ForegroundColor DarkYellow

            Push-Location "$relPath"
            $msbuildCmd = 'msbuild /nologo /verbosity:quiet /p:Configuration=__config__ /t:Rebuild ' + $prjFile

            $compDbg = $msbuildCmd.Replace( '__config__', 'Debug' )
            Write-Host $compDbg -ForegroundColor DarkGreen
            Invoke-Expression $compDbg | Out-Null

            $compRel = $msbuildCmd.Replace( '__config__', 'Release' )
            Write-Host $compRel -ForegroundColor DarkGreen
            Invoke-Expression $compRel | Out-Null
            Pop-Location

            Write-Host ""
        }

        Pop-Location

        Write-Host ""
    }
}


function Prep-Dependency()
{
    param(
        [string]$project,
        [string]$srcPath,
        [string]$dstPath
    )

    if ( $srcPath -eq $dstPath ) {
        Write-Host "Prep-Dependency: source and dest are the same, nothing to do" -ForegroundColor Yellow
        return
    }

    Write-Host "Preparing dependency " -ForegroundColor DarkCyan -NoNewline
    Write-Host "$project "  -ForegroundColor Green -NoNewline
    Write-Host "$srcPath -> $dstPath" -ForegroundColor DarkCyan

    $srcPrjPath = Join-Path -Path "$srcPath" -ChildPath ( Join-Path -Path "_deps" -ChildPath "$project-src" )
    $dstPrjPath = Join-Path -Path "$dstPath" -ChildPath ( Join-Path -Path "_deps" -ChildPath "$project-src" )

    $srcIncPath = Join-Path -Path "$srcPrjPath" -ChildPath "include"
    $dstIncPath = Join-Path -Path "$dstPrjPath" -ChildPath "include"

    if ( $project -eq "imgui" ) {
        $srcIncPath = "$srcPrjPath"
    } elseif ( $project -eq "zstd" ) {
        $srcIncPath = Join-Path -Path "$srcPrjPath" -ChildPath "lib"
    } elseif ( $project -eq "nfd" ) {
        $srcIncPath = Join-Path -Path "$srcPrjPath" -ChildPath ( Join-Path -Path "src" -ChildPath "include" )
    } elseif ( $project -eq "md4c" ) {
        $srcIncPath = Join-Path -Path "$srcPrjPath" -ChildPath "src"
    } elseif ( $project -eq "pugixml" ) {
        $srcIncPath = Join-Path -Path "$srcPrjPath" -ChildPath "src"
    }

    if ( Test-Path -Path "$srcIncPath" -PathType Container ) {
        New-Item -Path "$dstIncPath" -ItemType Directory -Force | Out-Null

        if ( $project -eq "imgui" ) {
            New-Item -Path "$dstIncPath\misc\freetype" -ItemType Directory -Force | Out-Null
            Copy-Item -Path "$srcIncPath\*.h" -Destination "$dstIncPath\" -Force
            Copy-Item -Path "$srcIncPath\misc\freetype\*.h" -Destination "$dstIncPath\misc\freetype" -Force

            $srcBackendPath = Join-Path -Path "$srcIncPath" -ChildPath "backends"
            $dstBackendPath = Join-Path -Path "$dstIncPath" -ChildPath "backends"

            if ( Test-Path -Path "$srcBackendPath" ) {
                New-Item -Path "$dstBackendPath" -ItemType Directory -Force | Out-Null

                Copy-Item -Path "$srcBackendPath\imgui_impl_glfw.*" -Destination "$dstBackendPath\" -Force
                Copy-Item -Path "$srcBackendPath\imgui_impl_opengl3.h" -Destination "$dstBackendPath\" -Force
                Copy-Item -Path "$srcBackendPath\imgui_impl_opengl3_loader.h" -Destination "$dstBackendPath\" -Force
            }
        } elseif ( $project -eq "zstd" ) {
            Copy-Item -Path "$srcIncPath\*.h" -Destination "$dstIncPath\" -Force
        } elseif ( $project -eq "md4c" ) {
            Copy-Item -Path "$srcIncPath\*.h" -Destination "$dstIncPath\" -Force
        } elseif ( $project -eq "usearch" ) {
            Copy-Item -Path "$srcPrjPath\include" -Destination "$dstPrjPath" -Container -Recurse -Force
            Copy-Item -Path "$srcPrjPath\fp16\include" -Destination "$dstPrjPath\" -Container -Recurse -Force
        } elseif ( $project -eq "pugixml" ) {
            Copy-Item -Path "$srcIncPath\*.h" -Destination "$dstIncPath\" -Force
            Copy-Item -Path "$srcIncPath\*.hpp" -Destination "$dstIncPath\" -Force
        } else {
            Copy-Item -Path "$srcIncPath\*" -Destination "$dstIncPath\" -Recurse -Container -Force
        }
    }

    if ( $project -eq "json" ) {
        Copy-Item -Path "$srcPrjPath\nlohmann_json.natvis" -Destination "$dstPrjPath\" -Force
    }

    $srcLibPath = Join-Path -Path "$srcPath" -ChildPath ( Join-Path -Path "_deps" -ChildPath "$project-build" )
    $dstLibPath = Join-Path -Path "$dstPath" -ChildPath ( Join-Path -Path "_deps" -ChildPath "$project-build" )

    if ( Test-Path -Path "$srcLibPath\debug" -PathType Container ) {
    } elseif ( Test-Path -Path "$srcLibPath\src\debug" -PathType Container ) {
        $srcLibPath = Join-Path -Path "$srcLibPath" -ChildPath "src"
    } elseif ( Test-Path -Path "$srcLibPath\lib\debug" -PathType Container ) {
        $srcLibPath = Join-Path -Path "$srcLibPath" -ChildPath "lib"
    }

    if ( Test-Path -Path "$srcLibPath\debug" -PathType Container ) {
        New-Item -Path "$dstLibPath\debug" -ItemType Directory -Force | Out-Null
        Copy-Item -Path "$srcLibPath\debug\*" -Destination "$dstLibPath\debug\" -Recurse -Container -Force
    }

    if ( Test-Path -Path "$srcLibPath\release" -PathType Container ) {
        New-Item -Path "$dstLibPath\release" -ItemType Directory -Force | Out-Null
        Copy-Item -Path "$srcLibPath\release\*" -Destination "$dstLibPath\release\" -Recurse -Container -Force
    }
}


function Copy-Dependencies
{
    param(
        [string]$srcPath,
        [string]$dstPath
    )

    if ( Test-Path -Path "$srcPath" -PathType Container ) {
        Write-Host "Copy external includes and libs $srcPath -> $dstPath" -ForegroundColor DarkCyan

        New-Item -Path "$dstPath" -ItemType Directory -Force | Out-Null

        $embedSrcDir = Join-Path -Path "$srcPath" -ChildPath "data"
        $embedDstDir = Join-Path -Path "$dstPath" -ChildPath "data"
        Write-Host "Copy generated embedded data from " $embedSrcDir " to " $embedDstDir
        Copy-Item -Path "$embedSrcDir" -Destination "$embedDstDir" -Force -Recurse

        $srcGitref = Join-Path -Path "$srcPath" -ChildPath "GitRef.hpp"
        Copy-Item -Path "$srcGitref" -Destination "$dstPath" -Force

        Write-Host "Handling external binary deps" -ForegroundColor DarkCyan
        foreach ( $proj in $externDeps ) {
            Prep-Dependency $proj $srcPath $dstPath
        }

        Write-Host ""
    }
}


function Get-MatchingNodes {
    param(
        [System.Xml.XmlDocument]$xml,
        [System.Xml.XmlNamespaceManager]$nsManager,
        [string]$xpath,
        [string]$nodeName,
        [string]$attr,
        [string[]]$filters
    )

    if ( -Not ( [string]::IsNullOrEmpty( $nodeName ) ) ) {
        $xpath = $xpath + "/ns:$nodeName"
    }
    $childNodes = $xml.SelectNodes($xpath, $nsManager)

    $matchingNodes = @()

    foreach ($node in $childNodes) {
        if ($node -is [System.Xml.XmlNode]) {
            $attrVal = $node.GetAttribute($attr)

            if ( $attrVal -eq $null -or $filters -eq $null -or $filters.Count -eq 0 ) {
                $matchingNodes += $node
            } else {
                foreach ($filter in $filters) {
                    if ($attrVal -like "*$filter*") {
                        $matchingNodes += $node
                        break
                    }
                }
            }
        }
    }

    return $matchingNodes
}


function Print-MatchingNodes {
    param(
        [System.Xml.XmlNode[]]$nodes
    )

    foreach ($node in $nodes) {
        Write-Host "Matching Node:" -ForegroundColor DarkGreen
        Write-Host $node.OuterXml
    }
}


function Remove-MatchingNodes {
    param(
        [System.Xml.XmlNode[]]$nodes
    )

    foreach ($node in $nodes) {
        $node.ParentNode.RemoveChild($node) | Out-Null
    }
}


function Rewrite-Projects
{
    param(
        [string]$srcPath,
        [string]$dstPath,
        [string]$binDst,
        [string]$exeName,
        [string]$extFilter,
        [string]$intDir
    )

    $depsFilter = $( $cmakeProjects; $externDeps )

    $srcRoot = Join-Path -Path "$PSScriptRoot" -ChildPath ""
    $srcPathAbs = Join-Path -Path "$PSScriptRoot" -ChildPath $srcPath
    $dstPathAbs = Join-Path -Path $srcRoot -ChildPath $dstPath
    $srcLevels = $srcRoot.TrimEnd( '\' ).Split( '\' )
    $dstLevels = $dstPathAbs.TrimEnd( '\' ).Split( '\' )
    $depthDiff = $dstLevels.Count - $srcLevels.Count
    $backSlashRepl = "..\" * $depthDiff
    $slashRepl = "../" * $depthDiff

    Get-ChildItem "$srcPath" -Filter $extFilter |
    Foreach-Object {
        $base = $_.Name.split(".",2)[0]

        # Just ignore the TracyImGui project. it's an "external dependency" so we build it and just use the lib
        if ( $base -eq 'TracyImGui' ) {
            return;
        }

        if ( -Not ( $cmakeProjects -contains $base ) ) {
            $inputFile = $_.FullName
            $inputName = $_.Name
            $outputPath = Join-Path -Path $PSScriptRoot -ChildPath $dstPath
            $outputFile = Join-Path -Path $outputPath -ChildPath $_.Name

            $content = Get-Content -Path $inputFile

            # Replace absolute source paths with absolute destination paths
            $content = $content.Replace( $srcPathAbs, $dstPathAbs )
            $content = $content.Replace( $srcPathAbs.Replace('\', '/'), $dstPathAbs )

            # Replace absolute paths with relative paths
            $content = $content.Replace( $srcRoot.Replace('\', '/'), $slashRepl)
            $content = $content.Replace( $srcRoot, $backSlashRepl )

            # It clearly is impossible to have a sane decent common directory structure...
            $content = $content.Replace( 'zstd-src\build\cmake\..\..\lib', 'zstd-src\include' )
            $content = $content.Replace( 'zstd-build\lib', 'zstd-build' )

            $content = $content.Replace( 'md4c-src\src', 'md4c-src\include' )
            $content = $content.Replace( 'md4c-build\src', 'md4c-build' )
            $content = $content.Replace( 'pugixml-src\src', 'pugixml-src\include' )
            $content = $content.Replace( 'pugixml-static', 'pugixml' )

            $content = $content.Replace( 'nfd-src\src\include', 'nfd-src\include' )
            $content = $content.Replace( 'nfd-build\src', 'nfd-build' )

            $content = $content.Replace( 'glfw-build\src', 'glfw-build' )

            $content = $content.Replace( 'libcurl-build\lib', 'libcurl-build' )

            $content = $content.Replace( '_deps\imgui-src', '_deps\imgui-src\include' )
            $content = $content.Replace( 'Debug\TracyImGui.lib', '_deps\imgui-build\Debug\TracyImGui.lib' )
            $content = $content.Replace( 'Release\TracyImGui.lib', '_deps\imgui-build\Release\TracyImGui.lib' )

            [XML]$xml = $content
            $nsUri = "http://schemas.microsoft.com/developer/msbuild/2003"

            $nsManager = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
            $nsManager.AddNamespace("ns", $nsUri)

            # Remove external project references
            $projRefNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:ItemGroup/ns:ProjectReference" -attr 'Include' -filters $depsFilter
            Remove-MatchingNodes -nodes $projRefNodes

            # Remove cmake references
            $cmakeRefNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:ItemGroup/ns:CustomBuild" -attr 'Include' -filters @( "CMakeLists.txt" )
            Remove-MatchingNodes -nodes $cmakeRefNodes

            $cmakeCustomBuildNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:ItemGroup/ns:CustomBuild" -attr 'Include' -filters @( ".rule" )
            Remove-MatchingNodes -nodes $cmakeCustomBuildNodes

            # Update intermediate directory
            $intNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:PropertyGroup/ns:IntDir"
            foreach ( $node in $intNodes ) {
                if ( $node -is [System.Xml.XmlNode] ) {
                    $nodeInt = Join-Path -Path $intDir -ChildPath $node.InnerXml
                    $node.InnerXml = $nodeInt
                }
            }

            # Update Windows SDK version
            $winSdkNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager "/ns:Project/ns:PropertyGroup/ns:WindowsTargetPlatformVersion"
            foreach ( $node in $winSdkNodes ) {
                if ($node -is [System.Xml.XmlNode]) {
                    $node.InnerXml = "10.0"
                }
            }

            # Add a post build step to copy the release executable to the target folder
            $cfgTypeNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:PropertyGroup/ns:ConfigurationType"
            if ( $cfgTypeNodes -and $cfgTypeNodes.Count -gt 0 ) {
                $cfg = $cfgTypeNodes[0]

                $isAppProj = ( ( $cfg -is [System.Xml.XmlNode] ) -and ( $cfg.InnerXml -eq 'Application' ) )
                $rewrite = $true
                if ( $rewrite -eq $true ) {
                    $prjFile = $_
                    Write-Host "Rewriting $prjFile"
                    if ( $isAppProj -and -not [string]::IsNullOrEmpty( $exeName ) ) {
                        $intNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:PropertyGroup/ns:TargetName" -attr 'Condition' -filters "Release"
                        foreach ( $node in $intNodes ) {
                            if ( $node -is [System.Xml.XmlNode] ) {
                                $node.InnerXml = $exeName
                            }
                        }
                    }

                    $itemDefGroupNodes = Get-MatchingNodes -xml $xml -nsManager $nsManager -xpath "/ns:Project/ns:ItemDefinitionGroup"
                    foreach( $itemDefGroup in $itemDefGroupNodes ) {
                        $cond = $itemDefGroup.GetAttribute( "Condition" )
                        if ( $cond -ne $null -and $cond -like  "*`$(Configuration)*==*Release*") {

                            $xmlCompile = $itemDefGroup.SelectSingleNode( "ns:ClCompile", $nsManager )
                            if ( $xmlCompile -ne $null ) {
                                $xmlDbgInfoFormat = $xmlCompile.SelectSingleNode( "ns:DebugInformationFormat", $nsManager )
                                if ( $xmlDbgInfoFormat -ne $null ) {
                                    $xmlDbgInfoFormat.ParentNode.RemoveChild($xmlDbgInfoFormat) | Out-Null
                                }

                                $xmlDbgInfoFormat = $xml.CreateElement( "DebugInformationFormat", $nsUri )
                                $xmlDbgInfoFormat.InnerXml = "ProgramDatabase"
                                $xmlCompile.AppendChild( $xmlDbgInfoFormat ) | Out-Null
                            }

                            if ( $isAppProj -eq $true ) {
                                $xmlLink = $itemDefGroup.SelectSingleNode( "ns:Link", $nsManager )
                                if ( $xmlLink -ne $null ) {
                                    $xmlGenDbg = $xmlLink.SelectSingleNode( "ns:GenerateDebugInformation", $nsManager )
                                    if ( $xmlGenDbg -ne $null ) {
                                        $xmlGenDbg.ParentNode.RemoveChild($xmlGenDbg) | Out-Null
                                    }

                                    $xmlGenDbg = $xml.CreateElement( "GenerateDebugInformation", $nsUri )
                                    $xmlGenDbg.InnerXml = "true"
                                    $xmlLink.AppendChild( $xmlGenDbg ) | Out-Null

                                    $xmlPdb = $xmlLink.SelectSingleNode( "ns:ProgramDataBaseFile", $nsManager )
                                    $xmlPdb.InnerXml = "`$(OutDir)`$(TargetName).pdb"
                                }

                                $xmlPostChild = $itemDefGroup.SelectSingleNode( "ns:PostBuildEvent", $nsManager )
                                if ( $xmlPostChild -ne $null ) {
                                    $xmlPostChild.ParentNode.RemoveChild($xmlPostChild) | Out-Null
                                }

                                $dstExecutablePath = "`$(ProjectDir)\$binDst"

                                $dstName = "`$(TargetName)"
                                $chkPath = "$dstExecutablePath\$dstName.*"
                                $borderLen = "**  **".Length
                                $outputLen = ("Make sure `"$chkPath`" is writable." | Measure-Object -Property Length -Maximum).Maximum + $borderLen
                                $linefailed = "Failed to copy output to destination."

                                $copyCmd = ""
                                $copyCmd += "`n"
                                $copyCmd += "setlocal`n"
                                $copyCmd += "echo.`n"
                                $copyCmd += "echo Copying build results to destination.`n"
                                $copyCmd += "echo.`n"
                                $copyCmd += "copy `"`$(OutDir)`$(TargetName).*`" `"$dstExecutablePath\$dstName.*`"`n"
                                $copyCmd += "echo.`n"
                                $copyCmd += "if ERRORLEVEL 1 (`n"
                                $copyCmd += "    echo FAILED`n"
                                $copyCmd += "    echo " + ("*" * $outputLen) + "`n"
                                $copyCmd += "    echo ** $linefailed" + (' ' * ($outputLen - $linefailed.Length - $borderLen)) + " **`n"
                                $copyCmd += "    echo ** Make sure `"$dstExecutablePath\$dstName.*`" is writable. **`n"
                                $copyCmd += "    echo " + ("*" * $outputLen) + "`n"
                                $copyCmd += ") else (`n"
                                $copyCmd += "    echo Succeeded`n"
                                $copyCmd += ")`n"
                                $copyCmd += "echo.`n"
                                $copyCmd += "endlocal`n"
                                $copyCmd += "exit /b 0`n"

                                $xmlPostChild = $xml.CreateElement( "PostBuildEvent", $nsUri )
                                $xmlCommandChild = $xml.CreateElement( "Command", $nsUri )
                                $xmlCommandChild.InnerXml = $copyCmd
                                $xmlPostChild.AppendChild( $xmlCommandChild ) | Out-Null
                                $itemDefGroup.AppendChild( $xmlPostChild ) | Out-Null
                            }
                        } elseif ( $cond -ne $null -and $cond -like  "*`$(Configuration)*==*Debug*") {
                            if ( $isAppProj -eq $true ) {
                                $xmlLink = $itemDefGroup.SelectSingleNode( "ns:Link", $nsManager )
                                if ( $xmlLink -ne $null ) {
                                    $xmlLinkOpts = $xmlLink.SelectSingleNode( "ns:AdditionalOptions", $nsManager )
                                    if ( $xmlLinkOpts -ne $null ) {
                                        $xmlLinkOpts.InnerXml += " /ignore:4099 "
                                    }
                                }
                            }
                        }

                            # break
                    }
                }
            }

            $xml.Save($outputFile)
        }
    }
}


function Copy-Projects
{
    param(
        [string]$srcPath,
        [string]$dstPath,
        [string]$binDst,
        [string]$exeName
    )

    Write-Host "Copy tracy projects and solution" -ForegroundColor DarkCyan
    New-Item -Path "$dstPath" -ItemType Directory -Force | Out-Null

    $intDir = "_temp"
    $intPath = Join-Path -Path "$dstPath" -ChildPath $intDir

    if ( Test-Path -Path "$intPath" -PathType Container ) {
        Remove-Item -Path "$intPath" -Recurse -Force | Out-Null
    }

    Rewrite-Projects "$srcPath" "$dstPath" "$binDst" "$exeName" "*.vcxproj" "$intDir"
    Rewrite-Projects "$srcPath" "$dstPath" "$binDst" "$exeName" "*.vcxproj.filters"

    Get-ChildItem "$srcPath" -Filter "*.sln" |
    Foreach-Object {
        $inputFile = $_.FullName
        $outputFile = Join-Path -Path "$dstPath" -ChildPath $_.Name
        Copy-Item -Path "$inputFile" -Destination "$outputFile" -Force

        $removedProjects = $( $cmakeProjects; $externDeps )
        $prjGuids = @()
        $prjNames = @()

        [string[]]$slnContent = Get-Content -Path "$outputFile"
        for ($i = 0; $i -lt $slnContent.Count; $i++) {
            $prjLine = $slnContent[ $i ]
            if ( $prjLine.TrimStart().StartsWith( "Project" ) ) {
                $excludePrj = $false

                foreach( $name in $removedProjects ) {
                    if ( $prjLine.ToUpper().Contains($name.ToUpper()) ) {
                        $excludePrj = $true
                        $prjParts = ( $prjLine -split "," )
                        if ( $prjParts.Count -ge 3 ) {
                            $name = (($prjParts[ 0 ] -split "=")[1]).Trim().Trim("`"")

                            if ( $name -eq "pugixml" ) {
                                $name += "-static"
                            }

                            $guid = $prjParts[ 2 ].Trim().Trim("`"").ToUpper()
                            if ( -not ( $prjGuids -contains $guid ) ) {
                                $prjGuids += $guid
                                $prjNames += $name
                            }
                        }
                    }
                }

                for ( ; $i -lt $slnContent.Count; $i++) {
                    $prjLine = $slnContent[ $i ]
                    if ( $prjLine.TrimStart().EndsWith( "EndProject" ) ) {
                        break
                    }
                }
            }
        }

        Write-Host "Removing"$prjGuids.Count"GUIDs from solution"
        for ($i = 0; $i -lt $prjGuids.Count; $i++) {
            $guid = $prjGuids[ $i ]
            $name = $prjNames[ $i ]
            Write-Host "$guid $name"
        }
        Write-Host ""

        $slnRewritten = @()
        for ($i = 0; $i -lt $slnContent.Count; $i++) {
            $prjLine = $slnContent[ $i ]



            if ( $prjLine.TrimStart().StartsWith( "Project" ) ) {
                $excludePrj = $false

                foreach( $guid in $prjGuids ) {
                    if ( $prjLine.ToUpper().Contains($guid) ) {
                        $excludePrj = $true
                    }
                }

                for ( ; $i -lt $slnContent.Count; $i++) {
                    $prjLine = $slnContent[ $i ]
                    $excludeLine = $false

                    foreach( $guid in $prjGuids ) {
                        if ( $prjLine.ToUpper().Contains($guid) ) {
                            $excludeLine = $true
                        }
                    }

                    if ( -not $excludePrj -and -not $excludeLine ) {
                        $slnRewritten += $prjLine
                    }

                    if ( $prjLine.TrimEnd().EndsWith( "EndProject" ) ) {
                        break
                    }
                }
            } elseif ( $prjLine.TrimStart().StartsWith( "Global" ) ) {
                for ( ; $i -lt $slnContent.Count; $i++) {
                    $excludePrj = $false
                    $prjLine = $slnContent[ $i ]

                    foreach( $guid in $prjGuids ) {
                        if ( $prjLine.ToUpper().Contains($guid) ) {
                            $excludePrj = $true
                        }
                    }

                    if ( -not $excludePrj ) {
                        $slnRewritten += $prjLine
                    }

                    if ( $prjLine.TrimEnd().EndsWith( "EndGlobal" ) ) {
                        break
                    }
                }
            } else {
                $slnRewritten += $prjLine
            }
        }

        Copy-Item -Path "$outputFile" -Destination "$outputFile.bak" -Force

        $slnRewritten = $slnRewritten -join "`n"
        $slnRewritten | Out-File -Encoding "UTF8" "$outputFile"
    }

    Write-Host ""
}


function Disable-DirectoryBuildProps
{
    param(
        [string]$dstPath
    )
    $propsXmlFile = Join-Path -Path $dstPath -ChildPath "Directory.Build.props"
    Remove-Item -Path $propsXmlFile -Force -erroraction silentlycontinue
    $propsXml = New-Object System.XMl.XmlTextWriter( $propsXmlFile, $null)
    $propsXml.WriteStartDocument()
    $propsXml.WriteStartElement( 'Project' )
    $propsXml.WriteEndElement()
    $propsXml.WriteEndDocument()
    $propsXml.Flush()
    $propsXml.Close()
}


function Make-TracyProject
{
    param(
        [string]$projectName,
        [string]$binDstRel,
        [string]$exeName
    )

    Write-Host ""
    Write-Host "Generating '$projectName'" -ForegroundColor Magenta
    Write-Host "Resulting binaries will be copied to '$binDstRel'"

    $projectDirAbs = Join-Path -Path "$PSScriptRoot" -ChildPath $projectName
    $source = "$projectName\build\_generated"

    $projectDst = $dest
    if ( [string]::IsNullOrEmpty( $projectDst ) ) {
        $projectDst = "$projectName\build\win32"
    }
    $destDirAbs = Join-Path -Path $PSScriptRoot -ChildPath $projectDst
    New-Item -Path "$destDirAbs" -ItemType Directory -Force | Out-Null

    $binDstAbs = Join-Path -Path "$PSScriptRoot" -ChildPath "$binDstRel"
    Push-Location "$destDirAbs"
    $binDst = Resolve-Path -Path "$binDstAbs" -Relative
    Pop-Location

    if ( Test-Path -Path "$source" -PathType Container ) {
        Remove-Item "$source" -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ( Test-Path -Path "$projectDst" -PathType Container ) {
        Remove-Item "$projectDst" -Recurse -Force -ErrorAction SilentlyContinue
    }

    New-Item -Path "$projectDst" -ItemType Directory -Force | Out-Null

    $success = $false
    $cmakeResult = (Cmake-GenerateProjects $source $projectName)
    if ( $cmakeResult -eq $true ) {
        Compile-Dependencies $source
        Copy-Dependencies $source $projectDst
        Copy-Projects $source $projectDst $binDst $exeName

        if ( -not $keepGenerated ) {
            Write-Host "Removing generated directory $source" -ForegroundColor Yellow
            Remove-Item -Path $source -Recurse -Force -erroraction silentlycontinue
        }

        $slnFiles = Get-ChildItem -Path $destDirAbs -Filter "*.sln"
        if ( $slnFiles.Length -gt 0 ) {
            $slnFile = $slnFiles[0]
            $destSlnAbs = Join-Path -Path $destDirAbs -ChildPath $slnFile
            Write-Host ""
            Write-Host "Solution generated in: " -NoNewline
            Write-Host "'$destSlnAbs'" -ForegroundColor Green
            Write-Host ""

            if ( $compile -eq $true ) {
                Push-Location "$destDirAbs"
                Write-Host "Building $slnFile Debug"
                & 'msbuild' '/nologo' '/verbosity:quiet' '/p:Configuration=Debug' '/t:Rebuild' $slnFile
                Write-Host "Building $slnFile Release"
                & 'msbuild' '/nologo' '/verbosity:quiet' '/p:Configuration=Release' '/t:Rebuild' $slnFile
                Pop-Location
            }
        }

        $success = $true
    }

    Write-Host "Generation of " -NoNewline
    Write-Host "'$projectName' " -NoNewline -ForegroundColor Magenta
    if ( $success -eq $true ) {
        Write-Host "succeeded" -ForegroundColor DarkGreen
    } else {
        Write-Host "failed!" -NoNewline -ForegroundColor Red
    }

    Write-Host ""
    return $success
}


function ServerapiGen-Header
{
    param(
        [string]$basePath,
        [string]$filename,
        [string]$commonPath,
        [string]$serverPath
    )

    New-Item -Path "$filename" -ItemType File | Out-Null

    $absFilename = Convert-Path -Path "$filename"
    $publicPath = Join-Path -Path "$basePath" -ChildPath "public"

    Push-Location (Split-Path -Path "$absFilename" -Parent)
    $relCommonPath = ( Resolve-Path -Path "$commonPath" -Relative ).Replace( '\', '/' )
    $relPublicPath = ( Resolve-Path -Path "$publicPath" -Relative ).Replace( '\', '/' )
    $relServerPath = ( Resolve-Path -Path "$serverPath" -Relative ).Replace( '\', '/' )
    Pop-Location

    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ============================================= */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* == This file was generated by generate.ps1 == */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ==              DO NOT MODIFY              == */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ============================================= */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#ifndef __TRACY_SERVER_API_HPP__" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#define __TRACY_SERVER_API_HPP__" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#pragma once" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#ifdef TRACY_ENABLE" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#include `"$relPublicPath/TracyFeatureDefines.h`"" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#if defined( TRACY_HAS_SERVER_API_SUPPORT )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma push_macro( `"NOMINMAX`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma push_macro( `"WIN32_LEAN_AND_MEAN`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      if !defined( NOMINMAX )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#          define NOMINMAX" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      endif // if !defined( NOMINMAX )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      if !defined( WIN32_LEAN_AND_MEAN )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#          define WIN32_LEAN_AND_MEAN" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      endif // if !defined( WIN32_LEAN_AND_MEAN )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( push, 1 )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( disable: 5262 )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   define TRACY_OVERRIDE_PROCESS_FORCE_INCLUDES 1" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   define TRACY_NO_CAPSTONE 1" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   define TRACY_NO_EXCEPTIONS 1" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   if !defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#       define NO_PARALLEL_SORT 1" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if !defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   if TRACY_OVERRIDE_PROCESS_FORCE_INCLUDES" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#       define tracy_force_inline_process" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if TRACY_OVERRIDE_PROCESS_FORCE_INCLUDES" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   define __TRACYSORT_HPP__" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   include `"$relServerPath/tracy_pdqsort.h`"" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    namespace ppqsort {" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    namespace execution {" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    class sequenced_policy {};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    class parallel_policy {};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    class sequenced_policy_force_branchless {};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    class parallel_policy_force_branchless {};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    inline constexpr sequenced_policy seq{};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    inline constexpr parallel_policy par{};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    inline constexpr sequenced_policy_force_branchless seq_force_branchless{};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    inline constexpr parallel_policy_force_branchless par_force_branchless{};" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    template<typename T, typename U>" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    inline constexpr bool _is_same_decay_v = std::is_same_v<std::decay_t<T>, std::decay_t<U>>;" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    } // namespace execution" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    template <typename ExecutionPolicy, typename RandomIt>" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    void sort( ExecutionPolicy&& policy, RandomIt begin, RandomIt end ) {" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "       ::tracy::pdqsort_branchless( begin, end );" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    }" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    template <typename ExecutionPolicy, typename RandomIt, typename Compare>" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    void sort( ExecutionPolicy&& policy, RandomIt begin, RandomIt end, Compare comp ) {" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "        ::tracy::pdqsort_branchless( begin, end, comp );" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    }" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "    } // namespace ppqsort" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    Get-ChildItem "$serverPath" -Filter "*.hpp" | Foreach-Object { "#   include `"$relServerPath/$_`"" | Out-File -Encoding ascii -FilePath "$filename" -Append }
    Get-ChildItem "$serverPath" -Filter "*.h"   | Foreach-Object { "#   include `"$relServerPath/$_`"" | Out-File -Encoding ascii -FilePath "$filename" -Append }
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma pop_macro( `"NOMINMAX`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma pop_macro( `"WIN32_LEAN_AND_MEAN`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( pop )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#endif // if defined( TRACY_HAS_SERVER_API_SUPPORT )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#endif // ifdef TRACY_ENABLE" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#endif // __TRACY_SERVER_API_HPP__" | Out-File -Encoding ascii -FilePath "$filename" -Append
}


function ServerapiGen-Source
{
    param(
        [string]$basePath,
        [string]$filename,
        [string]$commonPath,
        [string]$serverPath
    )

    New-Item -Path "$filename" -ItemType File | Out-Null

    $absFilename = Convert-Path -Path "$filename"
    $publicPath = Join-Path -Path "$basePath" -ChildPath "public"

    Push-Location (Split-Path -Path "$absFilename" -Parent)
    $relCommonPath = ( Resolve-Path -Path "$commonPath" -Relative ).Replace( '\', '/' )
    $relPublicPath = ( Resolve-Path -Path "$publicPath" -Relative ).Replace( '\', '/' )
    $relServerPath = ( Resolve-Path -Path "$serverPath" -Relative ).Replace( '\', '/' )
    Pop-Location

    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ============================================= */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* == This file was generated by generate.ps1 == */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ==              DO NOT MODIFY              == */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "/* ============================================= */" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#ifdef TRACY_ENABLE" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#include `"$relPublicPath/TracyFeatureDefines.h`"" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#if defined( TRACY_HAS_SERVER_API_SUPPORT )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   include `"TracyServerApiGen.hpp`"" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma push_macro( `"NOMINMAX`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma push_macro( `"WIN32_LEAN_AND_MEAN`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      if !defined( NOMINMAX )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#          define NOMINMAX" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      endif // if !defined( NOMINMAX )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      if !defined( WIN32_LEAN_AND_MEAN )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#          define WIN32_LEAN_AND_MEAN" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      endif // if !defined( WIN32_LEAN_AND_MEAN )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( push, 1 )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( disable: 5262 )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    $tracyStackFrames = Join-Path -Path "$commonPath" -ChildPath "TracyStackFrames.cpp"
    $tracyLz4Hc = Join-Path -Path "$commonPath" -ChildPath "tracy_lz4hc.cpp"
    if ( Test-Path -Path "$tracyStackFrames" ) {
        "//  IMPORTANT: This must be the opposite condition of the one found in TracyClient.cpp! (If you get link errors check it)" | Out-File -Encoding ascii -FilePath "$filename" -Append
        "#   if !defined(TRACY_HAS_CALLSTACK) || !(TRACY_HAS_CALLSTACK == 2 || TRACY_HAS_CALLSTACK == 3 || TRACY_HAS_CALLSTACK == 4 || TRACY_HAS_CALLSTACK == 6)" | Out-File -Encoding ascii -FilePath "$filename" -Append
        "#       include `"$relCommonPath/TracyStackFrames.cpp`"" | Out-File -Encoding ascii -FilePath "$filename" -Append
        "#   endif" | Out-File -Encoding ascii -FilePath "$filename" -Append
    }

    if ( Test-Path -Path "$tracyLz4Hc" ) {
        "" | Out-File -Encoding ascii -FilePath "$filename" -Append
        "#   include `"$relCommonPath/tracy_lz4hc.cpp`"" | Out-File -Encoding ascii -FilePath "$filename" -Append
    }
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   define tracy_zdict_include_relative" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    Get-ChildItem "$serverPath" -Filter "*.cpp" | Foreach-Object { "#   include `"$relServerPath/$_`"" | Out-File -Encoding ascii -FilePath "$filename" -Append }
    Get-ChildItem "$serverPath" -Filter "*.c"   | Foreach-Object { "#   include `"$relServerPath/$_`"" | Out-File -Encoding ascii -FilePath "$filename" -Append }
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#   if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma pop_macro( `"NOMINMAX`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma pop_macro( `"WIN32_LEAN_AND_MEAN`" )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#      pragma warning( pop )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "#   endif // if defined(_WIN32)" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#endif // if defined( TRACY_HAS_SERVER_API_SUPPORT )" | Out-File -Encoding ascii -FilePath "$filename" -Append
    "" | Out-File -Encoding ascii -FilePath "$filename" -Append

    "#endif // ifdef TRACY_ENABLE" | Out-File -Encoding ascii -FilePath "$filename" -Append
}


if ( $help ) {
    Write-Host "Usage: " $MyInvocation.MyCommand.Name " [-dest sln_output_path] [-compile] [-keepGenerated] [-profiler] [-capture] [-serverapi]" -ForegroundColor DarkCyan
    Write-Host "" -ForegroundColor DarkCyan
    Write-Host "  -dest           Output directory of the generated solution/project files (default: profiler/build/win32)" -ForegroundColor DarkCyan
    Write-Host "  -compile        (Attempts to) Compile the generated solutions in debug and release" -ForegroundColor DarkCyan
    Write-Host "  -keepGenerated  Do not delete the _generated folder" -ForegroundColor DarkCyan
    Write-Host "  -profiler       Generate the tracy profiler (default: yes)" -ForegroundColor DarkCyan
    Write-Host "  -capture        Generate the tracy capture program" -ForegroundColor DarkCyan

    Write-Host "  -serverapi      Generate the tracy server api lib" -ForegroundColor DarkCyan
} else {
    if ((Get-Command "cl.exe" -ErrorAction SilentlyContinue) -eq $null) {
        $defaultVs = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\Launch-VsDevShell.ps1'
        if ( Test-Path -Path $defaultVs ) {
            Invoke-Expression "& '$defaultVs'"
            Set-Location -Path $PSScriptRoot
        }
    }

    if ((Get-Command "cl.exe" -ErrorAction SilentlyContinue) -eq $null) {
        Write-Host ""
        Write-Host "Please run " -NoNewline
        Write-Host "%YOUR_VS_INSTALL_DIR%\Common7\Tools\Launch-VsDevShell.ps1" -ForegroundColor Red -NoNewline
        Write-Host " first"
        Write-Host "Usage: " $MyInvocation.MyCommand.Name " [-dest sln_output_path]" -ForegroundColor DarkCyan
        Write-Host ""
        Write-Host "Alternatively: " -NoNewline
        Write-Host "open Visual Studio Command Prompt (or manually run vcvars64.bat in cmd.exe)" -ForegroundColor Red -NoNewline
        Write-Host " first"
        Write-Host  "Usage: powershell -File " $MyInvocation.MyCommand.Name " [-dest sln_output_path]" -ForegroundColor DarkCyan
        Write-Host ""
        Exit 1
    } else {
        $binDstRel = "..\..\devtools\bin\win64"

        if (-not $profiler -and -not $capture -and -not $serverapi) {
            $profiler = $true
        }

        if ( $profiler -eq $true ) {
            $serverapi = $true
        }

        $profilerSuccess = $true
        $captureSuccess = $true

        # Make sure we ignore any Directory.Build.props file higher up in the directory structure
        Disable-DirectoryBuildProps $PSScriptRoot

        if ( $profiler ) {
            $profilerSuccess = Make-TracyProject "profiler" "$binDstRel" -exeName "Tracy"
        }

        if ( $capture -or $makeAll ) {
            $captureSuccess = Make-TracyProject "capture" "capture"
        }

        if ( $serverapi -or $makeAll ) {
            $basePath = "$PSScriptRoot"
            $apiCpp = Join-Path -Path "$PSScriptRoot" -ChildPath "serverapi/TracyServerApiGen.cpp"
            $apiHpp = Join-Path -Path "$PSScriptRoot" -ChildPath "serverapi/TracyServerApiGen.hpp"

            if ( Test-Path -Path "$apiCpp" -PathType Leaf ) {
                Remove-Item -Path "$apiCpp" -Force | Out-Null
            }

            if ( Test-Path -Path "$apiHpp" -PathType Leaf ) {
                Remove-Item -Path "$apiHpp" -Force | Out-Null
            }

            $commonPath = Join-Path -Path "$PSScriptRoot" -ChildPath "public/common"
            $serverPath = Join-Path -Path "$PSScriptRoot" -ChildPath "server"

            ServerapiGen-Header $basePath $apiHpp $commonPath $serverPath
            ServerapiGen-Source $basePath $apiCpp $commonPath $serverPath
        }

        if ( -not $profilerSuccess ) {
            Write-Host "Generating 'profiler' failed" -ForegroundColor Red
        }

        if ( -not $captureSuccess ) {
            Write-Host "Generating 'capture' failed" -ForegroundColor Red
        }

        if ( $profSuccess -and $captSuccess -and $serverapi ) {
            Write-Host "Press any key to quit..." -ForegroundColor Red
        } else {
            Write-Host "Press any key to quit..." -ForegroundColor DarkGreen
        }

        cmd /c pause | out-null
    }
}
