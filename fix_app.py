import os

file_path = r'c:\Users\PRATIRUPA\Downloads\SmartPlant\backend\backend\app.py'
with open(file_path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

new_lines = []
found = False

for line in lines:
    if 'app.run(host="0.0.0.0", port=5000, debug=True)' in line:
        indent = line[:line.find('app.run')]
        new_lines.append(f"{indent}import socket\n")
        new_lines.append(f"{indent}try:\n")
        new_lines.append(f"{indent}    s_ip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)\n")
        new_lines.append(f"{indent}    s_ip.connect((\"8.8.8.8\", 80))\n")
        new_lines.append(f"{indent}    local_ip = s_ip.getsockname()[0]\n")
        new_lines.append(f"{indent}    s_ip.close()\n")
        new_lines.append(f"{indent}    print(f\"\\n[SERVER] SmartPlant Running at: http://{{local_ip}}:5000\")\n")
        new_lines.append(f"{indent}    print(f\"[HARDWARE] Set ESP32 API_URL to: http://{{local_ip}}:5000/api/esp/sensor\\n\")\n")
        new_lines.append(f"{indent}except Exception:\n")
        new_lines.append(f"{indent}    print(\"[SERVER] Started on all interfaces (port 5000)\")\n")
        new_lines.append(f"{indent}\n")
        new_lines.append(line)
        found = True
    else:
        new_lines.append(line)

if found:
    with open(file_path, 'w', encoding='utf-8') as f:
        f.writelines(new_lines)
    print("Successfully updated app.py")
else:
    print("Could not find app.run line")
