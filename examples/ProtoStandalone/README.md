# Proto Standalone

Приклад M6: самостійна 3D-програма з матеріалами, динамічним світлом і C++.

1. Відкрийте `Launch-Standalone.cmd`.
2. Для перевірки в редакторі натисніть **Play**. WASD рухає куб, Space підіймає його.
3. Для окремої програми натисніть **Експорт .exe**.
4. Запустіть `Build/Proto Standalone/Proto Standalone.exe`. Переносіть усю папку збірки.
5. **F1** у Player відкриває графіку; **Escape** закриває Player.

Spin обертає куб, MoveLight рухає теплу лампу, KeyboardMove обробляє клавіатуру.
ReadData читає `Assets/Data/StandaloneMessage.txt` під час OnStart через GetAssetBytes.
Його сталий AssetId — `9f21b5d0-7f62-4e93-a8b3-68d2c4f11022`; він явно включений
у `build.additionalAssets`.

У журналі запуску є рядок:

```text
ReadData loaded 9f21b5d0-7f62-4e93-a8b3-68d2c4f11022: Proto Standalone raw payload v1
```

Авторський проєкт складається з `project.proto.json`, `Assets`, `Scenes`,
`Code`, `Config`, `Notices` та відтворюваного кешу `.proto`. Файли `.meta`
зберігають UUID при перенесенні ресурсів. Цей приклад має власні UUID проєкту
й сцени; попередній Proto Playground залишається окремим прикладом M5.

[Інструкція експорту та графіки](../../docs/M6_GUIDE.md).