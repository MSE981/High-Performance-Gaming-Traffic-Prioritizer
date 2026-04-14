const express = require('express');
const session = require('express-session');
const path = require('path');
const si = require('systeminformation');
const { exec, execFile } = require('child_process');
const os = require('os');
const app = express();

const PORT = 8888;

// === Global Configurations ===
let adminPassword = 'admin123';

let systemConfig = { traffic_mode: 'gaming', bandwidth_limit: 100, target_port: 27015 };
let extraFeaturesConfig = { dmz_ip: '', timezone: 'Asia/Shanghai' };

app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));
app.use(express.urlencoded({ extended: true }));

// === NEW: Global Template Variables (Accessible in all .ejs files) ===
app.locals.systemVersion = 'v1.0.4-gaming-core';
app.locals.currentYear = new Date().getFullYear();

app.use(session({
    secret: 'super_secret_gaming_key',
    resave: false,
    saveUninitialized: true
}));

const requireAuth = (req, res, next) => {
    if (req.session.loggedIn) next();
    else res.redirect('/login');
};

// === Routes ===
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
        const hours = Math.floor(time.uptime / 3600);
        const minutes = Math.floor((time.uptime % 3600) / 60);
        const seconds = Math.floor(time.uptime % 60);

        res.json({
            cpu: Math.round(cpu.currentLoad),
            memory: Math.round((mem.active / mem.total) * 100),
            uptime: `${hours}h ${minutes}m ${seconds}s`,
            download_speed: Math.floor(Math.random() * 200) + 300,
            connections: [{ source: '192.168.1.100:54321', dest: '104.160.131.3:27015', proto: 'UDP', rule: 'Gaming Priority', up: 15, down: 45, time: '00:15:22' }]
        });
    } catch (error) { console.error(error); res.status(500).json({ error: 'Failed to fetch hardware data' }); }
});

app.get('/interfaces', requireAuth, (req, res) => { res.render('interfaces'); });

// Add /services route
app.get('/services', requireAuth, (req, res) => { res.render('services'); });

app.post('/api/ping', requireAuth, (req, res) => {
    let target = req.body.target || '8.8.8.8';
    // Sanitize target to prevent shell injection
    target = target.replace(/[^a-zA-Z0-9.-]/g, '');
    if (!target) return res.json({ status: 'error', output: 'Invalid target' });
    
    const pingFlag = os.platform() === 'win32' ? '-n' : '-c';
    execFile('ping', [pingFlag, '4', target], (error, stdout, stderr) => {
        res.json({ status: error ? 'error' : 'success', output: stderr || stdout || error.message });
    });
});

app.get('/qos', requireAuth, (req, res) => { res.render('qos', { config: systemConfig }); });

app.post('/update_qos_settings', requireAuth, (req, res) => {
    systemConfig.traffic_mode = req.body.mode;
    systemConfig.bandwidth_limit = parseInt(req.body.limit, 10);
    if (req.body.target_port) systemConfig.target_port = parseInt(req.body.target_port, 10);
    res.redirect('/qos');
});

app.get('/devices', requireAuth, (req, res) => {
    res.render('devices', { devices: [{ ip: '192.168.1.100', mac: '00:1A:2B:3C:4D:5E', name: 'My-Gaming-PC', status: 'Optimized' }] });
});

app.get('/logs', requireAuth, (req, res) => {
    res.render('logs', { logs: [{ timestamp: new Date().toLocaleString(), level: 'INFO', message: 'System running normally.' }] });
});

// Mock endpoints for new features
app.post('/api/services/toggle', requireAuth, (req, res) => { res.json({ status: 'success' }); });
app.post('/api/interfaces/apply', requireAuth, (req, res) => { res.json({ status: 'success' }); });


app.get('/dev', requireAuth, (req, res) => { res.render('dev'); });

app.post('/api/terminal', requireAuth, (req, res) => {
    const command = req.body.command || '';
    if (!command) return res.json({ status: 'error', output: 'No command provided.' });
    exec(command, { timeout: 15000 }, (error, stdout, stderr) => {
        res.json({ status: 'success', output: stdout || stderr || 'Executed.' });
    });
});

app.get('/security', requireAuth, (req, res) => { res.render('security', { extraConfig: extraFeaturesConfig }); });

app.post('/api/change_password', requireAuth, (req, res) => {
    if (req.body.old_password === adminPassword) {
        adminPassword = req.body.new_password;
        req.session.loggedIn = false;
        res.send("Password updated! Please <a href='/login'>login again</a>.");
    } else {
        res.send("Incorrect current password. <a href='/security'>Go back</a>");
    }
});

app.post('/api/update_dmz', requireAuth, (req, res) => {
    if (req.body.dmz_enable === 'on') { extraFeaturesConfig.dmz_ip = req.body.dmz_ip; }
    else { extraFeaturesConfig.dmz_ip = ''; }
    res.redirect('/security');
});

app.post('/api/update_time', requireAuth, (req, res) => {
    extraFeaturesConfig.timezone = req.body.timezone;
    res.redirect('/security');
});

app.post('/api/check_update', requireAuth, (req, res) => {
    setTimeout(() => {
        res.send(`
            <div style="font-family:sans-serif; text-align:center; padding: 50px; background:#1a1a1a; color:white; height:100vh;">
                <h2 style="color:#28a745;">You are up to date!</h2>
                <p>Version v1.0.4-gaming-core is the latest release.</p>
                <a href="/security" style="color:#17a2b8;">Return to Settings</a>
            </div>
        `);
    }, 1500);
});

app.post('/api/reboot', requireAuth, (req, res) => {
    exec('sudo reboot', () => { });
    res.send('System rebooting...');
});

app.listen(PORT, () => { console.log(`Server is running at http://localhost:${PORT}`); });