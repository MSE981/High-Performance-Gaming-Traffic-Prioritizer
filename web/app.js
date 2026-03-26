const express = require('express');
const session = require('express-session');
const path = require('path');
const si = require('systeminformation');
const { exec } = require('child_process');
const os = require('os');
const app = express();

const PORT = 5000;

// NEW: Global Configuration Object (Acts as our central brain for settings)
let systemConfig = {
    traffic_mode: "gaming",
    bandwidth_limit: 100,
    target_port: 27015
};

app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));
app.use(express.urlencoded({ extended: true }));

app.use(session({
    secret: 'super_secret_gaming_key',
    resave: false,
    saveUninitialized: true
}));

const requireAuth = (req, res, next) => {
    if (req.session.loggedIn) {
        next();
    } else {
        res.redirect('/login');
    }
};

app.get('/', (req, res) => {
    res.redirect('/dashboard');
});

app.get('/login', (req, res) => {
    res.render('login');
});

app.post('/api/login', (req, res) => {
    const { password } = req.body;
    if (password === 'admin123') {
        req.session.loggedIn = true;
        res.redirect('/dashboard');
    } else {
        res.send('Access Denied: Incorrect password.');
    }
});

app.get('/dashboard', requireAuth, (req, res) => {
    res.render('dashboard');
});

app.get('/api/status', requireAuth, async (req, res) => {
    try {
        const cpu = await si.currentLoad();
        const mem = await si.mem();
        const time = si.time();

        const uptimeSeconds = time.uptime;
        const hours = Math.floor(uptimeSeconds / 3600);
        const minutes = Math.floor((uptimeSeconds % 3600) / 60);
        const seconds = Math.floor(uptimeSeconds % 60);

        const mockConnections = [
            { source: "192.168.1.100:54321", dest: "104.160.131.3:27015", proto: "UDP", rule: "Gaming Priority", up: 15, down: 45, time: "00:15:22" },
            { source: "192.168.1.101:44322", dest: "142.250.190.46:443", proto: "TCP", rule: "Streaming Mode", up: 120, down: 850, time: "01:05:10" }
        ];

        res.json({
            cpu: Math.round(cpu.currentLoad),
            memory: Math.round((mem.active / mem.total) * 100),
            uptime: `${hours}h ${minutes}m ${seconds}s`,
            download_speed: Math.floor(Math.random() * 200) + 300,
            connections: mockConnections
        });
    } catch (error) {
        res.status(500).json({ error: 'Failed to fetch hardware data' });
    }
});

app.get('/network', requireAuth, (req, res) => {
    res.render('network');
});

app.post('/api/ping', requireAuth, (req, res) => {
    const target = req.body.target || '8.8.8.8';
    const isWindows = os.platform() === 'win32';
    const pingFlag = isWindows ? '-n' : '-c';
    const command = `ping ${pingFlag} 4 ${target}`;

    exec(command, (error, stdout, stderr) => {
        if (error) {
            return res.json({ status: "error", output: stderr || stdout || error.message });
        }
        res.json({ status: "success", output: stdout });
    });
});

// NEW: Render the QoS page, passing the current global configuration
app.get('/qos', requireAuth, (req, res) => {
    res.render('qos', { config: systemConfig });
});

// NEW: Handle POST request to update QoS settings
app.post('/update_qos_settings', requireAuth, (req, res) => {
    // Update the global settings based on user input
    systemConfig.traffic_mode = req.body.mode;
    systemConfig.bandwidth_limit = parseInt(req.body.limit, 10);

    if (req.body.target_port) {
        systemConfig.target_port = parseInt(req.body.target_port, 10);
    }

    console.log("QoS Settings Updated:", systemConfig);

    // Redirect back to the QoS page to show updated values
    res.redirect('/qos');
});

app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});