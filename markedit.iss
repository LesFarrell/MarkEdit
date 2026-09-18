; Inno Setup script for MarkEdit.
; Build markedit.exe first (build.bat), then compile this with Inno Setup
; (ISCC.exe markedit.iss) from the repo root.
; build.bat stages the complete release tree in dist/. User settings, the
; WebView2 browser profile, debug symbols, import libraries, and compiler
; objects stay out.

#define MyAppName "MarkEdit"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "MarkEdit contributors"
#define MyAppExeName "markedit.exe"

[Setup]
AppId={{B2B0C9C0-5B0E-4C8B-9C7C-2C6C6E2C9A10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputBaseFilename=MarkEditSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "assocmd"; Description: "Associate .md files with {#MyAppName}"; GroupDescription: "File associations:"

[Files]
; Application runtime.
Source: "dist\markedit.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\Scintilla.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\Lexilla.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\WebView2Loader.dll"; DestDir: "{app}"; Flags: ignoreversion
; Offline preview assets.
Source: "dist\preview_shell.html"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\github-markdown.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\mermaid.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-powershell.min.js"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-github.min.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\highlight-github-dark.min.css"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\katex\*"; DestDir: "{app}\katex"; Flags: ignoreversion recursesubdirs createallsubdirs
; Application and redistributed-component licences.
Source: "dist\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\third_party\cmark-gfm\COPYING"; DestDir: "{app}\third_party\cmark-gfm"; Flags: ignoreversion
Source: "dist\third_party\highlightjs\LICENSE"; DestDir: "{app}\third_party\highlightjs"; Flags: ignoreversion
Source: "dist\third_party\highlightjs\VERSION.txt"; DestDir: "{app}\third_party\highlightjs"; Flags: ignoreversion
Source: "dist\third_party\katex\LICENSE"; DestDir: "{app}\third_party\katex"; Flags: ignoreversion
Source: "dist\third_party\katex\VERSION.txt"; DestDir: "{app}\third_party\katex"; Flags: ignoreversion
Source: "dist\third_party\lexilla\License.txt"; DestDir: "{app}\third_party\lexilla"; Flags: ignoreversion
Source: "dist\third_party\lexilla\version.txt"; DestDir: "{app}\third_party\lexilla"; Flags: ignoreversion
Source: "dist\third_party\scintilla\License.txt"; DestDir: "{app}\third_party\scintilla"; Flags: ignoreversion
Source: "dist\third_party\scintilla\version.txt"; DestDir: "{app}\third_party\scintilla"; Flags: ignoreversion
Source: "dist\third_party\webview2\LICENSE.txt"; DestDir: "{app}\third_party\webview2"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.md\OpenWithProgids"; ValueType: string; ValueName: "MarkEdit.md"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\MarkEdit.md"; ValueType: string; ValueName: ""; ValueData: "Markdown Document"; Flags: uninsdeletekey; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\MarkEdit.md\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: assocmd
Root: HKA; Subkey: "Software\Classes\MarkEdit.md\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocmd

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\webview2_data"
; get_settings_path() (settings.c) writes here, under %APPDATA%, not {app} -
; the app is never installed with write access to Program Files, so settings
; were never going to end up next to the exe.
Type: files; Name: "{userappdata}\markedit\markedit.ini"
