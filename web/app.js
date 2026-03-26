const express = require('express');
const session = require('express-session');
const path = require('path');
const si = require('systeminformation'); // NEW: Hardware monitoring library
const app = express();

const PORT = 5000;

// Configure EJS as the template engine
app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));

// Middleware to parse form data
app.use(express.urlencoded({ extended: true }));

// Configure session middleware
app.use(session({
    secret: 'super_secret_gaming_key',
    resave: false,
    saveUninitialized: true
}));

// Authentication Middleware
const requireAuth = (req, res, next) => {
    if (req.session.loggedIn) {
        next();
    } else {
        res.redirect('/login');
    }
};

// Root route redirects to dashboard
app.get('/', (req, res) => {
    res.redirect('/dashboard');
});

// Serve the login page
app.get('/login', (req, res) => {
    res.render('login');
});

// Handle login form submission
app.post('/api/login', (req, res) => {
    const { password } = req.body;
    if (password === 'admin123') {
        req.session.loggedIn = true;
        res.redirect('/dashboard');
    } else {
        res.send('Access Denied: Incorrect password.');
    }
});

// Protected dashboard route
app.get('/dashboard', requireAuth, (req, res) => {
    res.render('dashboard');
});

// NEW: API route to fetch real-time system status
app.get('/api/status', requireAuth, async (req, res) => {
    try {
        // Fetch real CPU and Memory data
        const cpu = await si.currentLoad();
        const mem = await si.mem();
        const time = si.time();

        // Calculate formatted uptime
        const uptimeSeconds = time.uptime;
        const hours = Math.floor(uptimeSeconds / 3600);
        const minutes = Math.floor((uptimeSeconds % 3600) / 60);
        const seconds = Math.floor(uptimeSeconds % 60);

        // Mock active connections (matching original C++ backend expectation)
        const mockConnections = [
            { source: "192.168.1.100:54321", dest: "104.160.131.3:27015", proto: "UDP", rule: "Gaming Priority", up: 15, down: 45, time: "00:15:22" },
            { source: "192.168.1.101:44322", dest: "142.250.190.46:443", proto: "TCP", rule: "Streaming Mode", up: 120, down: 850, time: "01:05:10" },
            { source: "192.168.1.105:33214", dest: "151.101.129.69:80", proto: "TCP", rule: "Default Route", up: 5, down: 12, time: "00:02:45" }
        ];

        // Send JSON response to the dashboard
        res.json({
            cpu: Math.round(cpu.currentLoad),
            memory: Math.round((mem.active / mem.total) * 100),
            uptime: `${hours}h ${minutes}m ${seconds}s`,
            download_speed: Math.floor(Math.random() * 200) + 300, // Simulate 300-500 KB/s
            connections: mockConnections
        });
    } catch (error) {
        res.status(500).json({ error: 'Failed to fetch hardware data' });
    }
});

// Start the server
app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});