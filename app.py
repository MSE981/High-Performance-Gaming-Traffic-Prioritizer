import time
import psutil
import json
from flask import Flask, render_template, request, redirect, url_for, session
from flask_socketio import SocketIO, emit
from threading import Thread

app = Flask(__name__)
app.secret_key = 'your_secret_key' # Used to encrypt session
socketio = SocketIO(app, cors_allowed_origins='*')

# Mock configuration parameters (should read from a file in reality)
system_config = {
    "traffic_mode": "gaming",  # Mode: gaming/streaming
    "bandwidth_limit": 100,    # Bandwidth limit in Mbps
    "target_port": 27015       # Game port
}

# --- 1. Background Monitoring Thread (Core: Get Network Speed) ---
def background_monitor():
    while True:
        # Get current network I/O (in bytes)
        net_io = psutil.net_io_counters()
        bytes_sent_before = net_io.bytes_sent
        bytes_recv_before = net_io.bytes_recv
        
        time.sleep(1) # 1-second interval
        
        net_io = psutil.net_io_counters()
        # Calculate speed per second (KB/s)
        upload_speed = (net_io.bytes_sent - bytes_sent_before) / 1024
        download_speed = (net_io.bytes_recv - bytes_recv_before) / 1024
        
        # Push to frontend via WebSocket
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
        # Hardcoded password for simplicity, use database/hash in reality
        if request.form['password'] == 'admin123':
            session['logged_in'] = True
            return redirect(url_for('index'))
        else:
            return "Incorrect password"
    return render_template('login.html')

@app.route('/update_config', methods=['POST'])
def update_config():
    if not session.get('logged_in'): return redirect(url_for('login'))
    
    # 1. Get new parameters submitted by frontend
    global system_config
    system_config['traffic_mode'] = request.form.get('mode')
    system_config['bandwidth_limit'] = request.form.get('limit')
    
    # 2. (Core) Write to the actual C++ config file here
    # with open('/etc/gaming-prioritizer/config.json', 'w') as f:
    #    json.dump(system_config, f)
    
    # 3. (Core) Restart C++ service
    # import subprocess
    # subprocess.run(["sudo", "systemctl", "restart", "gaming-service"])
    
    print(f"Configuration updated: {system_config}")
    return redirect(url_for('index'))

if __name__ == '__main__':
    # Start background monitoring
    Thread(target=background_monitor, daemon=True).start()
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)