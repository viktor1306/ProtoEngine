@echo off
setlocal
call "%~dp0..\..\Launch-Editor.cmd" --scene "%~dp0LightingLab.scene.json" --scene-camera
