param([Parameter(Mandatory=$true)][string]$ResultsDirectory,[Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
$m3Results=[IO.Path]::GetFullPath($ResultsDirectory)
$m3Manifest=Get-Content -LiteralPath (Join-Path $m3Results 'm3-lighting-results.json') -Raw | ConvertFrom-Json
if(-not $m3Manifest.passed){throw 'Motion report requires a passing native run.'}
$m3Frames=@()
foreach($m3Pose in 0..23) {
    $m3Index=$m3Pose*2
    $m3Pair=$m3Manifest.phases | Where-Object {$_.name -eq 'motion-sequence' -and $_.capture_index -eq ($m3Index+1)}
    if(-not $m3Pair.full_image_compared -or -not $m3Pair.passed){throw 'Missing paired motion comparison.'}
    $m3Cached=Join-Path $m3Results ('m3-lighting/motion-sequence-'+$m3Index+'-viewport.png')
    $m3Forced=Join-Path $m3Results ('m3-lighting/motion-sequence-'+($m3Index+1)+'-viewport.png')
    $m3Frames+=@{cached='data:image/png;base64,'+[Convert]::ToBase64String([IO.File]::ReadAllBytes($m3Cached));forced='data:image/png;base64,'+[Convert]::ToBase64String([IO.File]::ReadAllBytes($m3Forced));difference=$m3Pair.full_image_max_difference}
}
$m3Data=ConvertTo-Json -InputObject $m3Frames -Depth 4 -Compress
$m3Html=@'
<!doctype html><html lang="uk"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Proto Engine · Динамічні тіні M3</title>
<style>body{margin:0;background:#10141a;color:#e0e8ed;font:16px system-ui}main{max-width:1050px;margin:32px auto;padding:0 20px}h1{font-size:28px;margin:8px 0}p{color:#a9bac8;line-height:1.6}.tag{color:#73d5bc;letter-spacing:.14em;font-size:12px}img{display:block;width:100%;border:1px solid #34424e;border-radius:8px}nav{display:flex;flex-wrap:wrap;gap:18px;align-items:center;margin:18px 0}button{padding:10px 20px;background:#77d6bc;border:0;border-radius:5px;font:inherit;color:#10141a;cursor:pointer}input[type=range]{flex:1;min-width:180px;accent-color:#77d6bc}label{cursor:pointer}output{min-width:110px}.proof{padding:14px;background:#1b2530;border-radius:6px;color:#b4e0d6}small{color:#97a7b6}</style>
<main><div class="tag">PROTO ENGINE / M3</div><h1>Динамічні тіні в русі</h1><p>24 положення лампи, батьківських об'єктів і камери. Реальні зображення Vulkan: сонце, точкове світло та вирізи MASK. Фільтрація увімкнена; карти 512 px, три каскади.</p>
<img id="frame" alt="Кадр тестової 3D-сцени з динамічними тінями"><nav><button id="play">Відтворити</button><input id="position" type="range" min="0" max="23" value="0" aria-label="Положення сцени"><output id="number"></output></nav>
<nav><label><input type="radio" name="mode" value="cached" checked> Кешоване оновлення</label><label><input type="radio" name="mode" value="forced"> Примусове оновлення</label></nav><p class="proof" id="proof"></p><small>Це відтворення записаних кадрів. Його швидкість не є вимірюванням FPS рушія. PNG-пікселі з GPU збережені без переробки.</small></main>
<script>const frames=__FRAMES__;let pose=0,timer=null;const picture=document.getElementById('frame'),slider=document.getElementById('position'),play=document.getElementById('play');function show(){const mode=document.querySelector('input[name=mode]:checked').value;picture.src=frames[pose][mode];slider.value=pose;document.getElementById('number').textContent=`Положення ${pose+1} / 24`;document.getElementById('proof').textContent=`Повне порівняння двох кадрів цього положення: найбільша різниця RGB — ${frames[pose].difference} / 255.`;}slider.oninput=()=>{pose=Number(slider.value);show()};document.querySelectorAll('input[name=mode]').forEach(x=>x.onchange=show);play.onclick=()=>{if(timer){clearInterval(timer);timer=null;play.textContent='Відтворити'}else{timer=setInterval(()=>{pose=(pose+1)%frames.length;show()},180);play.textContent='Пауза'}};show();</script></html>
'@
$m3Target=[IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($m3Target)) | Out-Null
[IO.File]::WriteAllText($m3Target,$m3Html.Replace('__FRAMES__',$m3Data),[Text.UTF8Encoding]::new($false))
Write-Output $m3Target
