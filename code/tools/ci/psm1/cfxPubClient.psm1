using module .\cfxBuildContext.psm1
using module .\cfxBuildTools.psm1
using module .\cfxCacheVersions.psm1
using module .\cfxVersions.psm1

function Invoke-PubClient {
    param(
        [CfxBuildContext] $Context
    )

    # Host Tools
    $HostTools = "C:\Host_Tools\"
    $GenTool = "C:\Host_Tools\Generate_Manifest.rb"
    $UploadTool = "C:\Host_Tools\index"
    $UpdateTool = "C:\Host_Tools\Update_Host_Info.rb"

    $cacheName = "fivereborn"

    $binRoot = $Context.MSBuildOutput
    $packRoot = [IO.Path]::Combine($Context.CachesRoot, $cacheName)
    $cachesRoot = $Context.CachesRoot
    $projectRoot = $Context.ProjectRoot

    Invoke-EnsureDirExists $packRoot\bin\cef
    Invoke-EnsureDirExists $packRoot\citizen

    Push-Location $cachesRoot
    Start-Process powershell "ruby $GenTool $cachesRoot" -NoNewWindow -Wait
    Pop-Location
    Push-Location $HostTools
    Start-Process powershell "node $UploadTool" -NoNewWindow -Wait
    Start-Process powershell "ruby $UpdateTool $cachesRoot" -NoNewWindow -Wait
    Pop-Location

}