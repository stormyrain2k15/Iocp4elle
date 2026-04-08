Project: native_app

This is a minimal Visual C++ project configured for Win32 with MASM support and a Lua script.

Files created:
- main.cpp        (C++ entry point, calls assembly function `add`)
- startup.asm     (MASM source implementing `add`)
- lua_scripts/init.lua
- native_app.vcxproj (MSBuild project file)

Open native_app\native_app.vcxproj in Visual Studio (or open the folder) and build the Win32 Debug configuration.
