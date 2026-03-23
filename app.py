import time
import psutil
import json
import subprocess
import platform
from flask import Flask, render_template, request, redirect, url_for, session, jsonify
from flask_socketio import SocketIO, emit
from threading import Thread

app = Flask(__name__)
app.secret_key = 'your_secret_key' 
socketio = SocketIO(app, cors_allowed_origins='*')

# Mock configuration parameters
system_config = {
    "traffic_mode": "gaming",  
    "bandwidth_limit": 100,    
    "target_port": 27015       
}

# --- 1. Background Monitoring Thread ---
def background_monitor():
    while True:
        net_io = psutil.net_io_counters()
        bytes_sent_before = net_io.bytes_sent
        bytes_recv_before = net_io.bytes_recv
        
        time.sleep(1) 
        
        net_io = psutil.net_io_counters()
        upload_speed = (net_io.bytes_sent - bytes_sent_before) / 1024
        download_speed = (net_io.bytes_recv - bytes_recv_before) / 1024
        
        socketio.emit('net_update', {
            'upload': round(upload_speed, 2),
            'download': round(download_speed, 2)
        })

# --- 2. Routes and Control ---
@app.route('/')
def index():
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    return render_template('dashboard.html', config=system_config)

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        if request.form['password'] == 'admin123':
            session['logged_in'] = True
            return redirect(url_for('index'))
        else:
            return "Incorrect password"
    return render_template('login.html')

@app.route('/update_config', methods=['POST'])
def update_config():
    if not session.get('logged_in'): 
        return redirect(url_for('login'))
    
    global system_config
    system_config['traffic_mode'] = request.form.get('mode')
    system_config['bandwidth_limit'] = request.form.get('limit')
    
    print(f"Configuration updated: {system_config}")
    return redirect(url_for('index'))

# --- 3. New Network Diagnostics Routes ---
@app.route('/network')
def network_diagnostics():
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    return render_template('network.html')

@app.route('/api/ping', methods=['POST'])
def execute_ping():
    if not session.get('logged_in'):
        return jsonify({"status": "error", "message": "Unauthorized"}), 401
    
    target = request.form.get('target', '8.8.8.8')
    
    try:
        # Check OS to use correct ping parameter (-n for Windows, -c for Linux/Mac)
        param = '-n' if platform.system().lower() == 'windows' else '-c'
        result = subprocess.run(
            ['ping', param, '4', target],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10 
        )
        
        if result.returncode == 0:
            return jsonify({"status": "success", "output": result.stdout})
        else:
            return jsonify({"status": "error", "output": result.stderr or result.stdout})
    
    except subprocess.TimeoutExpired:
        return jsonify({"status": "error", "output": "Request timed out."})
    except Exception as e:
        return jsonify({"status": "error", "output": str(e)})
# --- 4. Traffic QoS Routes ---
@app.route('/qos')
def qos_page():
    # Security check
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    
    # Render the QoS page and pass the current configuration to it
    return render_template('qos.html', config=system_config)

@app.route('/update_qos_settings', methods=['POST'])
def update_qos_settings():
    # Security check
    if not session.get('logged_in'): 
        return redirect(url_for('login'))
    
    # Update the global configuration dictionary
    global system_config
    system_config['traffic_mode'] = request.form.get('mode')
    system_config['bandwidth_limit'] = request.form.get('limit')
    
    # Update the target game port if the user provided one
    new_port = request.form.get('target_port')
    if new_port and new_port.isdigit():
        system_config['target_port'] = int(new_port)
        
    print("QoS Settings Updated:", system_config)
    
    # Refresh the page to show the saved settings
    return redirect(url_for('qos_page'))

# --- 5. Connected Devices Route ---
@app.route('/devices')
def devices_page():
    # Security check: only logged-in users
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    
    # Mock data representing devices on your local network.
    # In a real-world scenario, you would use tools like 'scapy' or 'nmap' 
    # or parse the 'arp -a' command to get real network devices.
    connected_devices = [
        {"ip": "192.168.1.100", "mac": "00:1A:2B:3C:4D:5E", "name": "My-Gaming-PC", "status": "Optimized"},
        {"ip": "192.168.1.101", "mac": "A1:B2:C3:D4:E5:F6", "name": "Living-Room-TV", "status": "Streaming"},
        {"ip": "192.168.1.105", "mac": "11:22:33:44:55:66", "name": "Unknown-Mobile", "status": "High Traffic"}
    ]
    
    return render_template('devices.html', devices=connected_devices)

# --- 6. System Logs Route ---
@app.route('/logs')
def logs_page():
    # Security check: only logged-in users
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    
    # Mock log data. In a real application, you would read this from 
    # a real log file (e.g., /var/log/syslog) or a database like SQLite.
    system_logs = [
        {"timestamp": "2026-03-10 09:15:22", "level": "INFO", "message": "System booted successfully. Engine version 1.0.4."},
        {"timestamp": "2026-03-10 09:16:05", "level": "WARNING", "message": "High background traffic detected on IP 192.168.1.105."},
        {"timestamp": "2026-03-10 09:20:12", "level": "INFO", "message": "Admin user logged in from 127.0.0.1."},
        {"timestamp": "2026-03-10 09:35:00", "level": "ERROR", "message": "Failed to connect to primary DNS server. Retrying..."},
        {"timestamp": "2026-03-10 10:05:45", "level": "INFO", "message": "QoS settings updated: Mode set to Gaming, Limit 100Mbps."}
    ]
    
    return render_template('logs.html', logs=system_logs)

# --- 7. Developer Mode (Web Terminal) ---
@app.route('/dev')
def dev_page():
    # Security check
    if not session.get('logged_in'):
        return redirect(url_for('login'))
    return render_template('dev.html')

@app.route('/api/terminal', methods=['POST'])
def execute_command():
    # Strict security check: only authenticated users can run system commands
    if not session.get('logged_in'):
        return jsonify({"status": "error", "output": "Unauthorized access."}), 401
    
    command = request.form.get('command', '')
    if not command:
        return jsonify({"status": "error", "output": "No command provided."})
    
    try:
        # WARNING: shell=True allows executing arbitrary system commands.
        # This is powerful but dangerous. Use only in trusted local networks!
        result = subprocess.run(
            command,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15
        )
        
        # Return standard output, or standard error if the command failed
        output = result.stdout if result.stdout else result.stderr
        if not output:
            output = "[Command executed successfully with no output]"
        
        return jsonify({"status": "success", "output": output})
    
    except subprocess.TimeoutExpired:
        return jsonify({"status": "error", "output": "Command execution timed out."})
    except Exception as e:
        return jsonify({"status": "error", "output": "System error: " + str(e)})

# --- 8. API Status Route (For Dashboard Real-time Data) ---
@app.route('/api/status', methods=['GET'])
def get_system_status():
    # Security check: only logged-in users can access this data
    if not session.get('logged_in'):
        return jsonify({"error": "Unauthorized"}), 401
    
    # 1. Fetch REAL CPU and Memory usage using psutil
    # interval=0.1 ensures we get a fast but accurate CPU reading
    cpu_usage = psutil.cpu_percent(interval=0.1)
    
    memory_info = psutil.virtual_memory()
    memory_usage = memory_info.percent
    
    # 2. Calculate REAL System Uptime
    boot_time = psutil.boot_time()
    uptime_seconds = int(time.time() - boot_time)
    hours, remainder = divmod(uptime_seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    uptime_str = f"{hours}h {minutes}m {seconds}s"
    
    # 3. Mock Active Connections for the bottom table
    # (In a real scenario, this would come from the C++ network engine)
    mock_connections = [
        {"source": "192.168.1.100:54321", "dest": "104.160.131.3:27015", "proto": "UDP", "rule": "Gaming Priority", "up": 15, "down": 45, "time": "00:15:22"},
        {"source": "192.168.1.101:44322", "dest": "142.250.190.46:443", "proto": "TCP", "rule": "Streaming Mode", "up": 120, "down": 850, "time": "01:05:10"},
        {"source": "192.168.1.105:33214", "dest": "151.101.129.69:80", "proto": "TCP", "rule": "Default Route", "up": 5, "down": 12, "time": "00:02:45"}
    ]
    
    # Send all data back to dashboard.html in JSON format
    return jsonify({
        "cpu": cpu_usage,
        "memory": memory_usage,
        "uptime": uptime_str,
        "download_speed": 450, 
        "connections": mock_connections
    })

if __name__ == '__main__':
    Thread(target=background_monitor, daemon=True).start()
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)
