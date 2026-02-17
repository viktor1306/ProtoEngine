Set-Location 'c:\Programs\Programming\PythonProject\ProtoEngine'
git add -A
git status --short
git commit -m "feat: Voxel Chunk System — Hidden Face Culling, AO, ChunkManager, voxel pipeline"
git push origin main
Write-Host "=== DONE ==="
git log --oneline -4
