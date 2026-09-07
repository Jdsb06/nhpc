import subprocess
import time
import re
import matplotlib.pyplot as plt

# Configuration
REMOTE_HOST = "nhpc"
REMOTE_DIR = "IMT2024022"
REMOTE_IP = "172.16.201.12"
LOCAL_IP = "172.16.139.45"
PORT = "6969"
CHUNK_SIZE = "65536"

def parse_benchmark_output(raw_text):
    """Extracts size, time (usecs), and bandwidth (MB/s) from output table."""
    sizes, times, bws = [], [], []
    for line in raw_text.splitlines():
        # Match numeric data rows (e.g., '1024   2231.78   0.88')
        match = re.match(r"^\s*(\d+)\s+([\d\.]+)\s+([\d\.]+)", line)
        if match:
            sizes.append(int(match.group(1)))
            times.append(float(match.group(2)))
            bws.append(float(match.group(3)))
    return sizes, times, bws

# ----------------------------------------------------------------------
# 1. Run Direction 1: Local (Client) -> Remote (Server)
# ----------------------------------------------------------------------
print("[1/4] Starting Server on remote machine...")
ssh_server = subprocess.Popen(
    ["ssh", REMOTE_HOST, f"cd {REMOTE_DIR} && CHUNK_SIZE={CHUNK_SIZE} ./a1 server {PORT}"]
)
time.sleep(1.5)  # Allow server time to bind and listen

print("[2/4] Running Client locally...")
client_out_dir1 = subprocess.check_output(
    [f"./a1", "client", REMOTE_IP, PORT],
    env={"CHUNK_SIZE": CHUNK_SIZE, "PATH": "/usr/bin:/bin"}
).decode()

ssh_server.terminate()
time.sleep(1)

# ----------------------------------------------------------------------
# 2. Run Direction 2: Remote (Client) -> Local (Server)
# ----------------------------------------------------------------------
print("[3/4] Starting Server locally...")
local_server = subprocess.Popen(
    [f"./a1", "server", PORT],
    env={"CHUNK_SIZE": CHUNK_SIZE, "PATH": "/usr/bin:/bin"}
)
time.sleep(1.5)

print("[4/4] Running Client on remote machine via SSH...")
client_out_dir2 = subprocess.check_output(
    ["ssh", REMOTE_HOST, f"cd {REMOTE_DIR} && CHUNK_SIZE={CHUNK_SIZE} ./a1 client {LOCAL_IP} {PORT}"]
).decode()

local_server.terminate()

# ----------------------------------------------------------------------
# 3. Parse and Plot Results
# ----------------------------------------------------------------------
s1, t1, bw1 = parse_benchmark_output(client_out_dir1)
s2, t2, bw2 = parse_benchmark_output(client_out_dir2)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

# Latency Plot
ax1.plot(s1, t1, marker='o', label='Local -> Remote')
ax1.plot(s2, t2, marker='s', label='Remote -> Local')
ax1.set_xscale('log', base=2)
ax1.set_yscale('log')
ax1.set_title('TCP Latency vs Message Size')
ax1.set_xlabel('Message Size (Bytes)')
ax1.set_ylabel('Time (usecs) [Log Scale]')
ax1.grid(True, which="both", ls="--", alpha=0.5)
ax1.legend()

# Bandwidth Plot
ax2.plot(s1, bw1, marker='o', color='green', label='Local -> Remote')
ax2.plot(s2, bw2, marker='s', color='orange', label='Remote -> Local')
ax2.set_xscale('log', base=2)
ax2.set_title('TCP Bandwidth vs Message Size')
ax2.set_xlabel('Message Size (Bytes)')
ax2.set_ylabel('Bandwidth (MB/s)')
ax2.grid(True, which="both", ls="--", alpha=0.5)
ax2.legend()

plt.tight_layout()
plt.savefig("benchmark_comparison.png", dpi=300)
print("\nDone! Plots saved to benchmark_comparison.png")
plt.show()
