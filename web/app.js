const express = require('express');
const session = require('express-session');
const path = require('path');
const si = require('systeminformation');
const { execFile } = require('child_process');
const os = require('os');
const app = express();

const PORT = 5000;

let adminPassword = 'admin123'; // Make password dynamic

// Global Configuration
let systemConfig = {
    traffic_mode: "gaming",
    bandwidth_limit: 100,
    target_port: 27015
};

app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));
app.use(express.urlencoded({ extended: true }));

app.use(session({
    secret: process.env.SESSION_SECRET || 'change_this_secret',
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
    if (password === (process.env.ADMIN_PASSWORD || 'admin123')) {
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

    const safeTarget = String(target).trim();
    if (!/^[a-zA-Z0-9.\-:]+$/.test(safeTarget)) {
        return res.json({ status: "error", output: "Invalid target." });
    }

    execFile('ping', [pingFlag, '4', safeTarget], (error, stdout, stderr) => {
        if (error) {
            return res.json({ status: "error", output: stderr || stdout || error.message });
        }
        res.json({ status: "success", output: stdout });
    });
});

app.get('/qos', requireAuth, (req, res) => {
    res.render('qos', { config: systemConfig });
});

app.post('/update_qos_settings', requireAuth, (req, res) => {
    systemConfig.traffic_mode = req.body.mode;
    const limit = parseInt(req.body.limit, 10);
    if (!Number.isFinite(limit) || limit <= 0) {
        return res.status(400).send('Invalid bandwidth limit');
    }
    systemConfig.bandwidth_limit = limit;
    if (req.body.target_port) {
        const port = parseInt(req.body.target_port, 10);
        if (!Number.isFinite(port) || port < 1 || port > 65535) {
            return res.status(400).send('Invalid target port');
        }
        systemConfig.target_port = port;
    }
    console.log("QoS Settings Updated:", systemConfig);
    res.redirect('/qos');
});

// NEW: Devices Route
app.get('/devices', requireAuth, (req, res) => {
    const connectedDevices = [
        { ip: "192.168.1.100", mac: "00:1A:2B:3C:4D:5E", name: "My-Gaming-PC", status: "Optimized" },
        { ip: "192.168.1.101", mac: "A1:B2:C3:D4:E5:F6", name: "Living-Room-TV", status: "Streaming" },
        { ip: "192.168.1.105", mac: "11:22:33:44:55:66", name: "Unknown-Mobile", status: "High Traffic" }
    ];
    res.render('devices', { devices: connectedDevices });
});

// NEW: Logs Route
app.get('/logs', requireAuth, (req, res) => {
    const systemLogs = [
        { timestamp: "2026-03-10 09:15:22", level: "INFO", message: "System booted successfully. Engine version 1.0.4." },
        { timestamp: "2026-03-10 09:16:05", level: "WARNING", message: "High background traffic detected on IP 192.168.1.105." },
        { timestamp: "2026-03-10 09:20:12", level: "INFO", message: "Admin user logged in from 127.0.0.1." },
        { timestamp: "2026-03-10 09:35:00", level: "ERROR", message: "Failed to connect to primary DNS server. Retrying..." },
        { timestamp: "2026-03-10 10:05:45", level: "INFO", message: "QoS settings updated: Mode set to Gaming, Limit 100Mbps." }
    ];
    res.render('logs', { logs: systemLogs });
});

// NEW: Developer Terminal Route
app.get('/dev', requireAuth, (req, res) => {
    res.render('dev');
});

app.post('/api/terminal', requireAuth, (req, res) => {
    const command = String(req.body.command || '').trim();
    if (!command) {
        return res.json({ status: "error", output: "No command provided." });
    }

    const allowedCommands = {
        "uptime": ["uptime"],
        "df -h": ["df", "-h"],
        "df -h": ["df", "-h"],
        "free -h": ["free", "-h"],
    }
    if (!allowedCommands[command]) {
        return res.json({ status: "error", output: "Command not allowed." });
    }

    const [file, ...args] = allowedCommands[command];
    execFile(file, args, { timeout: 15000 }, (error, stdout, stderr) => {
        let output = stdout || stderr;
        if (error && !output) {
            output = "System error: " + error.message;
        }
        if (!output) {
            output = "[Command executed successfully with no output]";
        }
        res.json({ status: "success", output });
    });
});

// NEW: Render the More Features page
app.get('/more', requireAuth, (req, res) => {
    res.render('more');
});

// NEW: API to change admin password
app.post('/api/change_password', requireAuth, (req, res) => {
    const oldPassword = req.body.old_password;
    const newPassword = req.body.new_password;

    if (oldPassword === adminPassword) {
        adminPassword = newPassword;
        console.log("Admin password has been changed.");
        // Log user out so they must login with new password
        req.session.loggedIn = false;
        res.send("Password updated successfully! Please <a href='/login'>login again</a>.");
    } else {
        res.send("Error: Incorrect current password. <a href='/more'>Go back</a>");
    }
});

// NEW: API to reboot the system
app.post('/api/reboot', requireAuth, (req, res) => {
    console.log("System reboot initiated by user.");

    // Executes reboot command on Raspberry Pi (Linux)
    exec('sudo reboot', (error, stdout, stderr) => {
        if (error) {
            console.error(`Reboot error: ${error}`);
            return res.send("Error: Insufficient privileges to reboot.");
        }
    });

    res.send("System is rebooting. Please wait 1-2 minutes before refreshing the dashboard.");
});

app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});
