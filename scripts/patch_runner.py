import re, sys, os

path = sys.argv[1]
if not os.path.isfile(path):
    print("RunnerActivity not found!")
    import subprocess
    r = subprocess.run(['find', 'pcr_decompiled/smali', '-name', '*.smali'], capture_output=True, text=True)
    print(r.stdout[:2000])
    sys.exit(0)

pkg = "Lcom/StudioFurukawa/PixelCarRacer"

inject  = "    new-instance v0, Landroid/content/Intent;\n"
inject += "    const-class v1, " + pkg + "/ModOverlay;\n"
inject += "    invoke-direct {v0, p0, v1}, Landroid/content/Intent;-><init>(Landroid/content/Context;Ljava/lang/Class;)V\n"
inject += "    invoke-virtual {p0, v0}, " + pkg + "/RunnerActivity;->startService(Landroid/content/Intent;)Landroid/content/ComponentName;\n"

txt = open(path).read()
if "ModOverlay" in txt:
    print("Already patched, skipping.")
    sys.exit(0)

txt = re.sub(r'(invoke-super[^\n]*onCreate[^\n]*)', r'\1\n' + inject, txt, count=1)
open(path, 'w').write(txt)
print("RunnerActivity patched!")
