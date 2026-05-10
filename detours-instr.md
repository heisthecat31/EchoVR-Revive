# How to get detours.lib

Make sure you have the following:
- [git](https://git-scm.com/)
- [Visual Studio 2022](https://visualstudio.microsoft.com/) with "Desktop Development with C++" workload installed

1. Clone the repository
```cmd
git clone https://github.com/microsoft/Detours.git

cd Detours
```

2. Build it 

Open the ``x64 Native Tools Command Prompt for VS`` terminal

run:
```cmd
nmake
```


3. Add the file to this

After building, ``detours.lib`` should be in `lib.X64`. Grab the file from there and copy it into the root of this folder.