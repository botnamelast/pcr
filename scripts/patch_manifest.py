import sys

path = sys.argv[1]
txt  = open(path).read()

perm = '<uses-permission android:name="android.permission.SYSTEM_ALERT_WINDOW"/>'
svc  = '<service android:name="com.StudioFurukawa.PixelCarRacer.ModOverlay" android:enabled="true" android:exported="false"/>'

if perm not in txt:
    txt = txt.replace('<application', perm + '\n    <application', 1)
    print("Added SYSTEM_ALERT_WINDOW permission")

if svc not in txt:
    txt = txt.replace('</application>', '    ' + svc + '\n    </application>', 1)
    print("Added ModOverlay service")

open(path, 'w').write(txt)
print("Manifest patched!")
