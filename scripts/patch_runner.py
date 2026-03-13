import re, sys, os

path = sys.argv[1]
if not os.path.isfile(path):
    print("RunnerActivity not found!")
    import subprocess
    r = subprocess.run(['find', 'pcr_decompiled/smali', '-name', '*.smali'], capture_output=True, text=True)
    print(r.stdout[:2000])
    sys.exit(0)

pkg = "Lcom/StudioFurukawa/PixelCarRacer"
txt = open(path).read()

if "ModOverlay" in txt:
    print("Already patched, skipping.")
    sys.exit(0)

# Find onCreate method and its .locals count
# We need to add 2 locals (v_intent, v_class) safely
# Strategy: find .method onCreate, increase .locals by 2, inject at end before return

def patch_oncreate(txt):
    # Find onCreate method block
    method_pat = re.compile(
        r'(\.method[^\n]*onCreate\(Landroid/os/Bundle;\)V\n)'
        r'(.*?)'
        r'(\n\s*return-void)',
        re.DOTALL
    )
    m = method_pat.search(txt)
    if not m:
        print("ERROR: onCreate not found")
        return txt

    header   = m.group(1)
    body     = m.group(2)
    ret      = m.group(3)

    # Find .locals N
    locals_m = re.search(r'(\.locals\s+)(\d+)', body)
    if not locals_m:
        print("ERROR: .locals not found in onCreate")
        return txt

    old_locals = int(locals_m.group(2))
    new_locals = old_locals + 2
    body = body.replace(
        locals_m.group(0),
        locals_m.group(1) + str(new_locals)
    )

    # Use the two new registers
    va = "v" + str(old_locals)      # Intent instance
    vb = "v" + str(old_locals + 1)  # Class

    inject  = "\n"
    inject += "    new-instance " + va + ", Landroid/content/Intent;\n"
    inject += "    const-class " + vb + ", " + pkg + "/ModOverlay;\n"
    inject += "    invoke-direct {" + va + ", p0, " + vb + "}, Landroid/content/Intent;-><init>(Landroid/content/Context;Ljava/lang/Class;)V\n"
    inject += "    invoke-virtual {p0, " + va + "}, " + pkg + "/RunnerActivity;->startService(Landroid/content/Intent;)Landroid/content/ComponentName;\n"
    inject += "    move-result-object " + va + "\n"

    new_body = header + body + inject + ret
    return txt[:m.start()] + new_body + txt[m.end():]

txt = patch_oncreate(txt)
open(path, 'w').write(txt)
print("RunnerActivity patched!")
