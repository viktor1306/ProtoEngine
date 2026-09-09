# Порожній C++-проєкт Proto Engine

Шаблон створюється через `ProtoProject.exe` із цього комплекту.
Запустіть із цієї папки:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Create-Project.ps1 `
  -Destination 'C:\Work\My Proto Project' `
  -Name 'My Proto Project'
```

Батьківська папка повинна існувати, кінцева папка — бути новою.
Створюються нові UUID проєкту та стартової сцени, папки `Assets`, `Scenes`,
`Code`, `Config`, `.proto`, сцена з кубом/площиною/камерою/сонцем, порожня
реєстрація `Code/Behaviors.cpp` і налаштування графіки.
Підтримуються Windows PowerShell 5.1 та PowerShell 7. Python не потрібний.

Структура v0.1:

- `project.proto.json`
- `Assets/`
- `Scenes/`
- `Code/Behaviors.cpp`
- `Config/graphics.json`

Відкрийте редактор через `..\..\Launch-Editor.cmd`, виберіть **Файл → Відкрити
проєкт…** і створену папку. Приклади поведінок є в `Examples/ProtoM8Demo/Code`.
Після додавання класів не змінюйте їхні UUID без перенесення прив'язок у сцені.
Обирайте короткий шлях до проєкту; дуже глибокі шляхи можуть перевищити
ліміт Windows при створенні тимчасових файлів.
