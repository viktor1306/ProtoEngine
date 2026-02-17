Set-Location 'c:\Programs\Programming\PythonProject\ProtoEngine'
git add -A
git status --short
git commit -m "feat: ImGui integration, Timer, Camera refactor, Voxel World system (Culled Meshing)"
git push origin main
Write-Host "=== DONE ==="
git log --oneline -3
