const express = require('express');
const session = require('express-session');
const path = require('path');
const si = require('systeminformation');
const { exec } = require('child_process');
const os = require('os');
const app = express();

const PORT = 5000;

// Global Configurations
let adminPassword = 'admin123';

let systemConfig = {
    traffic_mode: "gaming",
    bandwidth_limit: 100,
    target_port: 27015
};

let wifiState = {
    ssid: "Gaming-Router-5G",
    password: "supersecretpassword",
    band: "dual",
    guest_network: false
};

// NEW: Extra configurations for DMZ and Timezone
let extraFeaturesConfig = {
    dmz_ip: "",
    timezone: "Asia/Shanghai"
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

app.get('/', (req, res) => { res.redirect('/dashboard'); });

app.get('/login', (req, res) => { res.render('login'); });

app.post('/api/login', (req, res) => {
    if (req.body.password === adminPassword) {
        req.session.loggedIn = true;
        res.redirect('/dashboard');
    } else {
        res.send('Access Denied: Incorrect password.');
    }
});

app.get('/dashboard', requireAuth, (req, res) => { res.render('dashboard'); });

app.get('/api/status', requireAuth, async (req, res) => {
    try {
        const cpu = await si.currentLoad();
        const mem = await si.mem();
        const time = si.time();
        const uptimeSeconds = time.uptime;
        const hours = Math.floor(uptimeSeconds / 3600);
        const minutes = Math.floor((uptimeSeconds % 3600) / 60);
        const seconds = Math.floor(uptimeSeconds % 60);

        res.json({
            cpu: Math.round(cpu.currentLoad),
            memory: Math.round((mem.active / mem.total) * 100),
            uptime: `${hours}h ${minutes}m ${seconds}s`,
            download_speed: Math.floor(Math.random() * 200) + 300,
            connections: [
                { source: "192.168.1.100:54321", dest: "104.160.131.3:27015", proto: "UDP", rule: "Gaming Priority", up: 15, down: 45, time: "00:15:22" }
            ]
        });
    } catch (error) {
        res.status(500).json({ error: 'Failed to fetch hardware data' });
    }
});

app.get('/network', requireAuth, (req, res) => { res.render('network'); });

app.post('/api/ping', requireAuth, (req, res) => {
    const target = req.body.target || '8.8.8.8';
    const pingFlag = os.platform() === 'win32' ? '-n' : '-c';
    exec(`ping ${pingFlag} 4 ${target}`, (error, stdout, stderr) => {
        res.json({ status: error ? "error" : "success", output: stderr || stdout || error.message });
    });
});

app.get('/qos', requireAuth, (req, res) => { res.render('qos', { config: systemConfig }); });

app.post('/update_qos_settings', requireAuth, (req, res) => {
    systemConfig.traffic_mode = req.body.mode;
    systemConfig.bandwidth_limit = parseInt(req.body.limit, 10);
    if (req.body.target_port) systemConfig.target_port = parseInt(req.body.target_port, 10);
    res.redirect('/qos');
});

app.get('/wifi', requireAuth, (req, res) => { res.render('wifi', { wifiConfig: wifiState }); });

app.post('/api/update_wifi', requireAuth, (req, res) => {
    wifiState.ssid = req.body.ssid;
    wifiState.password = req.body.password;
    wifiState.band = req.body.band;
    wifiState.guest_network = req.body.guest_network === 'on';
    res.redirect('/wifi');
});

app.get('/devices', requireAuth, (req, res) => {
    res.render('devices', { devices: [{ ip: "192.168.1.100", mac: "00:1A:2B...", name: "My-Gaming-PC", status: "Optimized" }] });
});

app.get('/logs', requireAuth, (req, res) => {
    res.render('logs', { logs: [{ timestamp: new Date().toLocaleString(), level: "INFO", message: "System running normally." }] });
});

app.get('/dev', requireAuth, (req, res) => { res.render('dev'); });

app.post('/api/terminal', requireAuth, (req, res) => {
    const command = req.body.command || '';
    if (!command) return res.json({ status: "error", output: "No command provided." });
    exec(command, { timeout: 15000 }, (error, stdout, stderr) => {
        res.json({ status: "success", output: stdout || stderr || "Executed." });
    });
});

// === MODIFIED MORE FEATURES ROUTES ===
app.get('/more', requireAuth, (req, res) => {
    // Pass the extra features configuration to the template
    res.render('more', { extraConfig: extraFeaturesConfig });
});

app.post('/api/change_password', requireAuth, (req, res) => {
    if (req.body.old_password === adminPassword) {
        adminPassword = req.body.new_password;
        req.session.loggedIn = false;
        res.send("Password updated! Please <a href='/login'>login again</a>.");
    } else {
        res.send("Incorrect current password. <a href='/more'>Go back</a>");
    }
});

// NEW: DMZ API
app.post('/api/update_dmz', requireAuth, (req, res) => {
    if (req.body.dmz_enable === 'on') {
        extraFeaturesConfig.dmz_ip = req.body.dmz_ip;
        console.log(`DMZ Enabled for IP: ${extraFeaturesConfig.dmz_ip}`);
    } else {
        extraFeaturesConfig.dmz_ip = "";
        console.log("DMZ Disabled.");
    }
    res.redirect('/more');
});

// NEW: Network Time API
app.post('/api/update_time', requireAuth, (req, res) => {
    extraFeaturesConfig.timezone = req.body.timezone;
    console.log(`System Timezone updated to: ${extraFeaturesConfig.timezone}`);
    // In Linux, you would run something like: exec(`sudo timedatectl set-timezone ${req.body.timezone}`)
    res.redirect('/more');
});

// NEW: Firmware Update Fake Check
app.post('/api/check_update', requireAuth, (req, res) => {
    console.log("Checking Huawei/Cloud servers for firmware update...");
    // Simulate a network delay then respond
    setTimeout(() => {
        res.send(`
            <div style="font-family:sans-serif; text-align:center; padding: 50px; background:#1a1a1a; color:white; height:100vh;">
                <h2 style="color:#28a745;">You are up to date!</h2>
                <p>Version v1.0.4-gaming-core is the latest release.</p>
                <a href="/more" style="color:#17a2b8;">Return to Settings</a>
            </div>
        `);
    }, 1500);
});

app.post('/api/reboot', requireAuth, (req, res) => {
    exec('sudo reboot', () => { });
    res.send("System rebooting...");
});

app.listen(PORT, () => { console.log(`Server is running at http://localhost:${PORT}`); });