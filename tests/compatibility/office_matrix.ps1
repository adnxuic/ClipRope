param(
    [Parameter(Mandatory)][string]$DesktopExe,
    [Parameter(Mandatory)][string]$MatrixExe,
    [Parameter(Mandatory)][string]$Report,
    [ValidateSet('Both','Excel','PowerPoint')][string]$Application='Both',
    [switch]$NativeOnly
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
Add-Type -Path (Join-Path $PSScriptRoot 'office_matrix_native.cs')
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not [MatrixDesktop]::PrivateStation()) {throw 'Run in the private test station.'}
$reportDir=[IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Report))
$folder=Join-Path $reportDir ('office-matrix-'+[guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($folder)
[IO.File]::WriteAllText($Report,'')
$fixtures=Join-Path $PSScriptRoot 'office_matrix_fixtures'
$script:sequence=0
function Record([string]$line) {
    [IO.File]::AppendAllText($Report,$line+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
    [Console]::WriteLine($line)
}
function Events([string]$path) {
    if(![IO.File]::Exists($path)){return}
    $f=[IO.File]::Open($path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($f)
    try{while(!$reader.EndOfStream){$reader.ReadLine()}}finally{$reader.Dispose()}
}
function Launch([string]$exe,[string]$arguments) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$exe; $start.Arguments=$arguments; $start.UseShellExecute=$false
    $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    return [Diagnostics.Process]::Start($start)
}
function Start-Office([string]$kind,[bool]$cold) {
    if($kind -eq 'PowerPoint' -and @(Get-Process POWERPNT -ErrorAction SilentlyContinue | Where-Object SessionId -eq ([Diagnostics.Process]::GetCurrentProcess().SessionId)).Count){
        throw 'An existing PowerPoint instance is active; refusing to redirect the synthetic presentation to it.'
    }
    $bin=if($kind -eq 'Excel'){'excel.exe'}else{'powerpnt.exe'}
    $ext=if($kind -eq 'Excel'){'xlsx'}else{'pptx'}
    $base=if($cold){'blank'}else{'sample'}
    $fixture=Join-Path $folder ("$kind-$base.$ext")
    [IO.File]::Copy((Join-Path $fixtures "$base.$ext"),$fixture,$true)
    $exe=(Get-Item ("Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\"+$bin)).GetValue('')
    $flags=if($kind -eq 'Excel'){'/x /safe'}else{'/safe'}
    $child=Launch $exe ($flags+' "'+$fixture+'"')
    try {
        $class=if($kind -eq 'Excel'){'EXCEL7'}else{'paneClassDC'}
        $until=[DateTime]::UtcNow.AddSeconds(35)
        do {
            if($child.HasExited){throw 'Dedicated Office process exited before binding'}
            $window=[MatrixDesktop]::Bind($child.Id,$class)
            if(!$window -and $kind -eq 'PowerPoint'){
                # Ask the running-object table only. Never activate or modify
                # an Office instance whose HWND is outside our process.
                $window=[MatrixDesktop]::Bind($child.Id,'mdiClass')
                try{
                    $opened=[MatrixDesktop]::RunningDocument($fixture)
                    if($opened -and [MatrixDesktop]::Pid([intptr]$opened.Application.HWND) -eq $child.Id -and $opened.FullName -eq $fixture){
                        $window=$opened.Windows.Item(1)
                    }
                }catch{}
                try{
                    $running=[Runtime.InteropServices.Marshal]::GetActiveObject('PowerPoint.Application')
                    if([MatrixDesktop]::Pid([intptr]$running.HWND) -eq $child.Id -and $running.Windows.Count -gt 0){$window=$running.ActiveWindow}
                }catch{}
            }
            if($window){
                try{
                    $loaded=if($kind -eq 'Excel'){$window.ActiveSheet.Parent.FullName}else{$window.Presentation.FullName}
                    if($loaded -eq $fixture){break}
                }catch{}
                $window=$null
            }
            Start-Sleep -Milliseconds 130
        }while([DateTime]::UtcNow -lt $until)
        if(!$window){throw ("Could not bind the dedicated $kind document window. Classes: "+[MatrixDesktop]::Classes($child.Id))}
        $app=$window.Application
        if($kind -eq 'Excel'){$doc=$window.ActiveSheet.Parent;$app.DisplayAlerts=$false}
        else{$doc=$window.Presentation;$app.DisplayAlerts=1}
        if($doc.FullName -ne $fixture){throw 'Refused unrelated Office document'}
        Record ("office kind=$kind cold=$cold pid="+$child.Id+" version="+[Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion)
        return [pscustomobject]@{Process=$child;Window=$window;App=$app;Doc=$doc;Kind=$kind}
    } catch {
        if(!$child.HasExited){$child.Kill();$child.WaitForExit()};throw
    }
}
function Close-Office($instance) {
    if(!$instance){return}
    try {
        [void][MatrixDesktop]::Clear([uint32]$instance.Process.Id,$script:sequence)
        $instance.Doc.Saved=$true
        $instance.App.Quit()
    } finally {
        if(!$instance.Process.WaitForExit(2500)){$instance.Process.Kill();$instance.Process.WaitForExit()}
        if(!$instance.Process.HasExited){throw 'Dedicated Office producer is still running'}
    }
}
function Start-Observer([string]$kind,[string]$epoch,[string]$store) {
    $log=Join-Path $folder ("events-"+$kind+"-"+$epoch+".txt")
    $child=Launch $DesktopExe ('--show --profile "'+$store+'" --probe "'+$log+'"')
    $until=[DateTime]::UtcNow.AddSeconds(10)
    try { do {
        if($child.HasExited){throw 'Isolated ClipRope listener exited'}
        if(@(Events $log) -contains 'ready') {
            return [pscustomobject]@{Process=$child;Log=$log}
        }
        Start-Sleep -Milliseconds 70
    } while([DateTime]::UtcNow -lt $until)
    throw 'Isolated ClipRope listener not ready'
    } catch {if(!$child.HasExited){$child.Kill();$child.WaitForExit()};throw}
}
function Close-Observer($observer) {
    if(!$observer){return}
    $h=[MatrixDesktop]::Listener($observer.Process.Id)
    if($h -ne [intptr]::Zero){[void][MatrixDesktop]::PostMessage($h,16,[intptr]::Zero,[intptr]::Zero)}
    if(!$observer.Process.WaitForExit(4000)){$observer.Process.Kill();$observer.Process.WaitForExit();throw 'Observer did not exit cleanly'}
}
function New-Destination($ins) {
    if($ins.Kind -eq 'Excel'){
        $doc=$ins.App.Workbooks.Add();$page=$doc.Worksheets.Item(1)
    }else{
        $doc=$ins.App.Presentations.Add(-1)
        $doc.PageSetup.SlideWidth=$ins.Doc.PageSetup.SlideWidth
        $doc.PageSetup.SlideHeight=$ins.Doc.PageSetup.SlideHeight
        $page=$doc.Slides.Add(1,12)
    }
    return [pscustomobject]@{Doc=$doc;Page=$page}
}
function Close-Destination($ins,$dest) {
    if($ins.Kind -eq 'Excel'){$dest.Doc.Close($false)}else{$dest.Doc.Saved=-1;$dest.Doc.Close()}
}
function Copy-Object($ins,[string]$case) {
    $before=[MatrixDesktop]::GetClipboardSequenceNumber()
    [void]$ins.Window.Activate()
    if($ins.Kind -eq 'Excel'){
        $sheet=$ins.Doc.Worksheets.Item(1);[void]$sheet.Activate()
        switch($case){
            values {$object=$sheet.Range('A1:B1');break}
            formulas {$object=$sheet.Range('A3:B3');break}
            merged {$object=$sheet.Range('A5:B5');break}
            chart {$object=$sheet.ChartObjects(1);break}
        }
        [void]$object.Select()
    }else{
        $index=@('text','group','chart','slide').IndexOf($case)+1
        $page=$ins.Doc.Slides.Item($index)
        if($case -eq 'slide'){
            $ins.Window.ViewType=7;$page.Select();$object=$page
        }else{
            $ins.Window.ViewType=9;$ins.Window.View.GotoSlide($index)
            $object=$page.Shapes.Item(1);$object.Select()
        }
    }
    $route='ribbon-copy'
    try{$ins.App.CommandBars.ExecuteMso('Copy')}catch{$route='object-copy';$object.Copy()}
    $until=[DateTime]::UtcNow.AddSeconds(3)
    do {
        $v=[MatrixDesktop]::GetClipboardSequenceNumber()
        if($v -ne $before -and [MatrixDesktop]::Owner() -eq $ins.Process.Id){
            [void][MatrixDesktop]::Seal([uint32]$ins.Process.Id,$v)
            $script:sequence=[MatrixDesktop]::GetClipboardSequenceNumber();return $route
        }
        Start-Sleep -Milliseconds 40
    }while([DateTime]::UtcNow -lt $until)
    throw 'Copy did not produce a new event owned by the verified source'
}
function Pixel-Error([string]$a,[string]$b) {
    if(![IO.File]::Exists($a) -or ![IO.File]::Exists($b)){return 'unmeasured'}
    $aa=[Drawing.Bitmap]::new($a);$bb=[Drawing.Bitmap]::new($b)
    try {
        $sum=0
        foreach($y in 0..31){foreach($x in 0..31){
            $ca=$aa.GetPixel([int][Math]::Floor(($x+.5)*$aa.Width/32),[int][Math]::Floor(($y+.5)*$aa.Height/32))
            $cb=$bb.GetPixel([int][Math]::Floor(($x+.5)*$bb.Width/32),[int][Math]::Floor(($y+.5)*$bb.Height/32))
            foreach($ch in 'R','G','B'){
                $va=255-(255-$ca.$ch)*$ca.A/255;$vb=255-(255-$cb.$ch)*$cb.A/255
                $sum+=[Math]::Abs($va-$vb)
            }
        }}
        return [Math]::Round($sum/(32*32*3),2)
    }finally{$aa.Dispose();$bb.Dispose()}
}
function Excel-Observation($dest,[string]$case,[string]$image) {
    $s=$dest.Page;$dest.Doc.Activate();$s.Activate();$at=$s.Range('A1');$at.Select()
    try{[void]$s.Paste()}catch{return [ordered]@{paste='rejected';code=$_.Exception.HResult}}
    $o=[ordered]@{paste='accepted';shapes=[int]$s.Shapes.Count;charts=[int]$s.ChartObjects().Count}
    if($case -eq 'values') {
        $o.values=($at.Value2 -ceq 'ClipRope values' -and $s.Range('B1').Value2 -eq 17)
    }elseif($case -eq 'formulas'){
        $o.values=($at.Value2 -eq 5 -and $s.Range('B1').Value2 -eq 42)
        $o.formulas=($at.Formula -eq '=2+3' -and $s.Range('B1').Formula -eq '=7*6')
    }elseif($case -eq 'merged'){
        $o.values=($at.Value2 -ceq 'ClipRope merged')
        $o.merged=[bool]$at.MergeCells -and $at.MergeArea.Columns.Count -eq 2
        $o.bold=[bool]$at.Font.Bold;$o.fill=($at.Interior.Color -eq 0x55ccf0)
    }elseif($case -eq 'chart'){
        if($o.charts -gt 0){
            $chart=$s.ChartObjects(1).Chart
            $o.title=($chart.HasTitle -and $chart.ChartTitle.Text -ceq 'ClipRope chart')
            $o.column=($chart.ChartType -eq 51)
            $series=$chart.SeriesCollection(1)
            $o.values=(@($series.Values) -join ',' -eq '3,7')
            [void]$chart.Export($image,'PNG')
            $chart.ChartTitle.Text='Edited title';$o.editableChart=$chart.ChartTitle.Text -ceq 'Edited title'
        }elseif($o.shapes){
            $sh=$s.Shapes.Item(1);$o.shapeType=[int]$sh.Type
            $o.shapeWidth=[Math]::Round($sh.Width);$o.shapeHeight=[Math]::Round($sh.Height)
            # Read this pasted shape's embedded pixels from the disposable
            # destination package. CopyPicture would replace the publication.
            $snapshot=$image+'.xlsx';$dest.Doc.SaveCopyAs($snapshot)
            $z=[IO.Compression.ZipFile]::OpenRead($snapshot)
            try{
                $media=$z.Entries|Where-Object {$_.FullName -match '^xl/media/[^/]+\.(png|jpe?g|bmp)$'}|Select-Object -First 1
                if($media){$stream=$media.Open();try{$bitmap=[Drawing.Bitmap]::new($stream);try{$bitmap.Save($image,[Drawing.Imaging.ImageFormat]::Png)}finally{$bitmap.Dispose()}}finally{$stream.Dispose()}}
            }finally{$z.Dispose()}
        }else{$o.cellContent=($null -ne $at.Value2)}
    }
    if($case -ne 'chart'){
        $had=$null -ne $at.Value2;$at.Formula='=3+4';$o.editableCell=$had -and $at.Value2 -eq 7
        if($o.shapes -gt 0){$o.shapeType=[int]$s.Shapes.Item(1).Type}
    }
    return $o
}
function Wait-PowerPointPaste($dest,[string]$case,[string]$route,[uint32]$owner) {
    $until=[DateTime]::UtcNow.AddSeconds(4)
    $readyCount=0
    $nextActivation=[DateTime]::UtcNow.AddMilliseconds(250)
    do {
        $active=$false;$enabled=$false
        try {
            $app=$dest.Doc.Application
            $active=($app.ActivePresentation.Name -ceq $dest.Doc.Name)
            if($active -and $case -ne 'slide') {
                $active=($app.ActiveWindow.View.Slide.SlideID -eq $dest.Page.SlideID)
            }
            # PNG-only restore onto the slide list is a measured native rejection.
            $enabled=($route -ne 'native' -and $case -eq 'slide') -or $app.CommandBars.GetEnabledMso('Paste')
        } catch {}
        # Ribbon Copy can asynchronously reactivate its source after the first Activate.
        # Restore only the destination created in this private Office instance. Keep
        # both document and slide identity checks in force before issuing any paste.
        if(!$active -and [DateTime]::UtcNow -ge $nextActivation) {
            $nextActivation=[DateTime]::UtcNow.AddMilliseconds(350)
            try {
                $target=$dest.Doc.Windows.Item(1)
                $target.Activate()
                if($case -eq 'slide') { $target.ViewType=7; $dest.Page.Select() }
                else { $target.ViewType=9; $target.View.GotoSlide($dest.Page.SlideIndex) }
            } catch {}
        }
        $available=[MatrixDesktop]::ClipboardReady($owner)
        if($active -and $enabled -and $available) {
            if(++$readyCount -ge 2){return}
        } else {$readyCount=0}
        Start-Sleep -Milliseconds 50
    } while([DateTime]::UtcNow -lt $until)
    throw "Paste readiness timed out (destination=$active enabled=$enabled clipboard=$available)"
}
function PowerPoint-Observation($dest,[string]$case,[string]$image,[string]$route,[uint32]$owner) {
    $win=$dest.Doc.Windows.Item(1);$win.Activate()
    if($case -eq 'slide'){
        $win.ViewType=7;$dest.Page.Select()
        $beforeSlides=$dest.Doc.Slides.Count;$beforeShapes=$dest.Page.Shapes.Count
        Wait-PowerPointPaste $dest $case $route $owner
        try{[void]$dest.Doc.Slides.Paste()}catch{return [ordered]@{paste='rejected';code=$_.Exception.GetBaseException().HResult}}
        $added=$dest.Doc.Slides.Count-$beforeSlides
        if($added -gt 0){$s=$dest.Doc.Slides.Item($dest.Doc.Slides.Count)}else{$s=$dest.Page}
        if($added -eq 0 -and $s.Shapes.Count -eq $beforeShapes){
            return [ordered]@{paste='no insertion';slides=0;shapes=0}
        }
        $s.Export($image,'PNG',640,480)
        $o=[ordered]@{paste='accepted';slides=$added;shapes=$s.Shapes.Count}
        if($s.Shapes.Count){$o.firstShapeType=[int]$s.Shapes.Item(1).Type}
        if($s.Shapes.Count -ge 2){
            $label=$s.Shapes.Item(1);$rect=$s.Shapes.Item(2)
            $o.text=$label.TextFrame.TextRange.Text -ceq 'ClipRope whole slide'
            $o.fill=$rect.Fill.ForeColor.RGB -eq 0x55ccf0
            $x=$rect.Left;$rect.Left=$x+12;$o.editableObjects=$rect.Left -gt $x
        }
        return $o
    }
    $win.ViewType=9;$win.View.GotoSlide(1)
    Wait-PowerPointPaste $dest $case $route $owner
    try{$added=$dest.Page.Shapes.Paste()}catch{return [ordered]@{paste='rejected';code=$_.Exception.GetBaseException().HResult}}
    $s=$added.Item(1)
    $o=[ordered]@{paste='accepted';shapes=$added.Count;type=[int]$s.Type;width=[Math]::Round($s.Width);height=[Math]::Round($s.Height)}
    try{$s.Export($image,2)}catch{}
    if($case -eq 'text' -and $s.HasTextFrame){
        $r=$s.TextFrame.TextRange;$o.text=$r.Text -ceq 'ClipRope text';$o.bold=$r.Font.Bold -eq -1
        $o.fontColor=$r.Font.Color.RGB -eq 0x554317;$r.Text='Edited text';$o.editableText=$r.Text -ceq 'Edited text'
    }elseif($case -eq 'group' -and $s.Type -eq 6){
        $o.children=$s.GroupItems.Count
        $o.labels=$s.GroupItems.Item(1).TextFrame.TextRange.Text -ceq 'One' -and $s.GroupItems.Item(2).TextFrame.TextRange.Text -ceq 'Two'
        $o.fill=$s.GroupItems.Item(1).Fill.ForeColor.RGB -eq 0x55ccf0
        $split=$s.Ungroup();$o.editableGroup=$split.Count -eq 2
    }elseif($case -eq 'chart' -and $s.HasChart){
        $chart=$s.Chart;$o.title=$chart.HasTitle -and $chart.ChartTitle.Text -ceq 'ClipRope chart'
        $o.column=$chart.ChartType -eq 51;$o.values=(@($chart.SeriesCollection(1).Values) -join ',' -eq '3,7')
        $chart.ChartTitle.Text='Edited title';$o.editableChart=$chart.ChartTitle.Text -ceq 'Edited title'
    }
    return $o
}
function Paste-Case($ins,$dest,[string]$case,[string]$route,[uint32]$owner) {
    $image=Join-Path $folder ($ins.Kind+'-'+$case+'-'+$route+'.png')
    try {
        $raw=if($ins.Kind -eq 'Excel'){Excel-Observation $dest $case $image}else{PowerPoint-Observation $dest $case $image $route $owner}
        $o=@($raw)[-1]
        if(!($o -is [Collections.IDictionary])){throw 'Invalid measurement'}
        if($route -eq 'native' -and $o.paste -ne 'accepted'){throw "Native baseline $($o.paste), paste HRESULT=$($o.code)"}
        if($route -eq 'native'){
            $expected=switch($case){
                values {'values','editableCell';break}
                formulas {'values','formulas','editableCell';break}
                merged {'values','merged','bold','fill','editableCell';break}
                chart {'values','column','title','editableChart';break}
                text {'text','bold','fontColor','editableText';break}
                group {'labels','fill','editableGroup';break}
                slide {'text','fill','editableObjects';break}
            }
            foreach($flag in $expected){if($o[$flag] -ne $true){throw "Native source identity check failed: $flag"}}
        }
        if($route -ne 'native' -and [IO.File]::Exists($image)){$o.pixelMae=Pixel-Error (Join-Path $folder ($ins.Kind+'-'+$case+'-native.png')) $image}
    } catch {
        Record ("measurement-error app="+$ins.Kind+" case=$case route=$route code="+$_.Exception.HResult+" line="+$_.InvocationInfo.ScriptLineNumber)
        throw
    }
    Record ("result app="+$ins.Kind+" case=$case route=$route "+($o|ConvertTo-Json -Compress))
}
function Restore-Head($observer,[string]$entry) {
    $ok=$false
    foreach($i in 1..12){if([MatrixDesktop]::Overwrite()){$ok=$true;break};Start-Sleep -Milliseconds 20}
    if(!$ok){throw 'Could not establish independent synthetic overwrite'}
    $script:sequence=[MatrixDesktop]::GetClipboardSequenceNumber()
    # The sentinel carries the local-history exclusion marker. It cannot
    # displace the saved Office item at the rope head.
    $before=@(Events $observer.Log).Count
    if(![MatrixDesktop]::Recall($observer.Process.Id)){throw 'Could not request resident head recall'}
    $until=[DateTime]::UtcNow.AddSeconds(7)
    do {
        $recent=@(Events $observer.Log|Select-Object -Skip $before)
        if($recent -contains "invoke head $entry published skipped committed" -and [MatrixDesktop]::Owner() -eq $observer.Process.Id){
            $script:sequence=[MatrixDesktop]::GetClipboardSequenceNumber()
            [void][MatrixDesktop]::Seal([uint32]$observer.Process.Id,$script:sequence)
            $offers=& $MatrixExe --published $observer.Process.Id
            if($LASTEXITCODE){throw 'Could not inspect resident publication'}
            foreach($line in $offers){Record $line}
            $script:publishedKeys=@($offers | Where-Object {$_ -match '^readable\s'} | ForEach-Object {$_.Substring(9)} | Sort-Object)
            return
        }
        if($recent -match '^invoke head .* failed'){throw 'Resident host reported a publication failure'}
        Start-Sleep -Milliseconds 40
    }while([DateTime]::UtcNow -lt $until)
    throw ('Resident host did not publish the expected bound entry. States: '+($recent -join ' / '))
}
function Check-OfficeSelection($ins) {
    $view=$null
    $classes=if($ins.Kind -eq 'Excel'){@('EXCEL7')}else{@('paneClassDC','mdiClass')}
    foreach($class in $classes) {
        $pane=[MatrixDesktop]::NativePane($ins.Process.Id,$class,[ref]$view)
        if($pane -ne [IntPtr]::Zero){break}
    }
    if(!$view){throw "No native document pane for $($ins.Kind)"}
    $start=[Diagnostics.ProcessStartInfo]::new($MatrixExe,"--selection "+$pane.ToInt64())
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardInput=$true
    $start.RedirectStandardOutput=$true
    $probe=[Diagnostics.Process]::Start($start)
    function Expect([string]$command,[string]$result,[string]$case) {
        $probe.StandardInput.WriteLine($command)
        $reply=$probe.StandardOutput.ReadLineAsync()
        if(!$reply.Wait(4000) -or $reply.Result -cne $result){throw "Selection $($ins.Kind) $case $command expected $result"}
        Record "selection app=$($ins.Kind) case=$case $command=$result"
    }
    try {
        if($ins.Kind -eq 'Excel') {
            $sheet=$ins.Doc.Worksheets.Item(1)
            foreach($address in @('C3','C3:D4','A5:B5')) {
                $sheet.Range($address).Select()
                Expect bind yes $address
                Expect verify yes $address
                $sheet.Range('C3:E4').Select()
                Expect verify no changed-range
            }
            $sheet.Range('C3:D4').Select()
            Expect bind yes active-cell
            $sheet.Range('D4').Activate()
            Expect verify no changed-active-cell
            $sheet.Range('C3').Select()
            $sheet.Protect()
            try { Expect bind no protected-cell } finally { $sheet.Unprotect() }
        }else{
            $ins.Window.ViewType=9
            $ins.Window.View.GotoSlide(1)
            $ins.Window.Selection.Unselect()
            Expect bind yes canvas
            Expect verify yes canvas
            $ins.Window.View.GotoSlide(2)
            Expect verify no changed-slide
            $ins.Window.View.GotoSlide(1)
            $shape=$ins.Doc.Slides.Item(1).Shapes.Item(1)
            $shape.Select()
            Expect bind yes shape
            Expect verify yes shape
            $shape.TextFrame.TextRange.Characters(1,2).Select()
            Expect verify no changed-kind
            Expect bind yes text
            Expect verify yes text
            $shape.TextFrame.TextRange.Characters(2,1).Select()
            Expect verify no changed-text
            Expect bind yes text-owner
            $other=$ins.Doc.Slides.Item(1).Shapes.AddTextbox(1,50,50,100,50)
            try {
                $other.TextFrame.TextRange.Text='Test'
                $other.TextFrame.TextRange.Characters(2,1).Select()
                Expect verify no changed-text-owner
            }finally{ $other.Delete() }
            $ins.Window.ViewType=7
            $ins.Doc.Slides.Item(1).Select()
            Expect bind yes slide-selection
            Expect verify yes slide-selection
            $ins.Doc.Slides.Item(2).Select()
            Expect verify no changed-thumbnail
        }
    }finally{
        $probe.StandardInput.Close()
        if(!$probe.WaitForExit(2000)){$probe.Kill();$probe.WaitForExit()}
        $probe.Dispose()
    }
}
function Matrix([string]$kind) {
    $cases=if($kind -eq 'Excel'){@('values','formulas','merged','chart')}else{@('text','group','chart','slide')}
    $ins=$null;$observer=$null;$entries=@{};$stores=@{};$byteKeys=@{}
    try {
        $ins=Start-Office $kind $false
        if(!$NativeOnly){Check-OfficeSelection $ins}
        foreach($case in $cases){
            $store=Join-Path $folder ("resident-$kind-$case")
            $stores[$case]=$store
            $observer=Start-Observer $kind ("warm-"+$case) $store
            $dest=New-Destination $ins
            try {
                $before=@(Events $observer.Log).Count
                $issued=Copy-Object $ins $case
                Record "copy app=$kind case=$case method=$issued"
                $until=[DateTime]::UtcNow.AddSeconds(4)
                do {
                    $states=@(Events $observer.Log|Select-Object -Skip $before|Where-Object {$_ -match '^capture '})
                    $entry=$null
                    foreach($state in $states){if($state -match '^capture (captured|partial) ([a-f0-9]{32})'){$entry=$Matches[2]}}
                    if($entry -or $states -match 'excluded|no-usable-format'){break}
                    Start-Sleep -Milliseconds 40
                }while([DateTime]::UtcNow -lt $until)
                if(!$NativeOnly) {
                    $explicit=Join-Path $folder ("controlled-$kind-$case")
                    $lines=& $MatrixExe $explicit $ins.Process.Id
                    $code=$LASTEXITCODE
                    foreach($line in $lines){Record $line}
                    if($code){throw "Format inventory failed for $kind $case"}
                    $benchmark=$null
                    foreach($line in $lines){if($line -match '^explicit-entry=([a-f0-9]{32})'){$benchmark=$Matches[1]}}
                }
                Paste-Case $ins $dest $case 'native' $ins.Process.Id
                Record ("passive app=$kind case=$case states="+(@($states|ForEach-Object {($_ -split ' ')[1]}) -join ','))
            } finally{
                [void][MatrixDesktop]::Clear([uint32]$ins.Process.Id,$script:sequence)
                Close-Destination $ins $dest
            }
            if(!$NativeOnly){
                if($entry){Record "profile app=$kind case=$case route=passive-resident"}
                elseif($benchmark){
                    Close-Observer $observer;$observer=$null
                    $stores[$case]=$explicit;$entry=$benchmark
                    $observer=Start-Observer $kind ("explicit-"+$case) $explicit
                    Record "profile app=$kind case=$case route=explicit-synthetic"
                }
                $entries[$case]=$entry
                if($entry){
                    $dest=New-Destination $ins
                    try {
                        Restore-Head $observer $entry
                        $byteKeys[$case]=$script:publishedKeys -join '|'
                        Paste-Case $ins $dest $case 'warm' $observer.Process.Id
                    } finally{
                        [void][MatrixDesktop]::Clear([uint32]$observer.Process.Id,$script:sequence)
                        Close-Destination $ins $dest
                    }
                }else{Record "result app=$kind case=$case route=warm no-saved-representation"}
            }
            Close-Observer $observer;$observer=$null
            if(!$NativeOnly -and $entries[$case]){
                $manifest=& $MatrixExe --manifest $stores[$case] $entries[$case]
                if($LASTEXITCODE){throw 'Could not reopen the resident manifest'}
                foreach($line in $manifest){Record $line}
                $savedKeys=@($manifest | Where-Object {$_ -match '^resident-format\s'} | ForEach-Object {$_.Substring(16)} | Sort-Object)
                if(($savedKeys -join '|') -cne $byteKeys[$case]){throw 'Saved and enumerated publication bytes differ'}
            }
        }
        Close-Office $ins;$ins=$null
        if(!$NativeOnly){
            $ins=Start-Office $kind $true
            foreach($case in $cases){
                $observer=Start-Observer $kind ("cold-"+$case) $stores[$case]
                $entry=$entries[$case]
                if($entry){
                    $dest=New-Destination $ins
                    try{
                        Restore-Head $observer $entry
                        if(($script:publishedKeys -join '|') -cne $byteKeys[$case]){throw 'Restart changed the readable publication'}
                        Paste-Case $ins $dest $case 'cold' $observer.Process.Id
                    }finally{
                        [void][MatrixDesktop]::Clear([uint32]$observer.Process.Id,$script:sequence)
                        Close-Destination $ins $dest
                    }
                }else{Record "result app=$kind case=$case route=cold no-saved-representation"}
                Close-Observer $observer;$observer=$null
            }
        }
    }finally{
        try{Close-Office $ins}finally{Close-Observer $observer}
    }
}
try {
    $os=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    Record "Windows=$($os.DisplayVersion) build=$($os.CurrentBuildNumber).$($os.UBR) nativeOnly=$NativeOnly"
    if($Application -ne 'PowerPoint'){Matrix 'Excel'}
    if($Application -ne 'Excel'){Matrix 'PowerPoint'}
    Record 'MATRIX-COMPLETE'
}finally{
    if($script:sequence -and ![MatrixDesktop]::Clear(0,$script:sequence)){throw 'Could not release verified matrix sample'}
    $created=[IO.DirectoryInfo]::new($folder)
    if($created.Exists -and $created.Parent.FullName -eq $reportDir -and $created.Name -match '^office-matrix-[0-9a-f-]{36}$'){
        try{[IO.Directory]::Delete($folder,$true)}catch{Record 'Temporary fixture directory held by a transient reader; retained locally.'}
    }
}
