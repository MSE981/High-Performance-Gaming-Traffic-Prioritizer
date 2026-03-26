const express = require('express');
const session = require('express-session');
const path = require('path');
const app = express();

const PORT = 5000;

// Configure EJS as the template engine
app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));

// Middleware to parse form data
app.use(express.urlencoded({ extended: true }));

// Configure session middleware (The 'memory' of our server)
app.use(session({
    secret: 'super_secret_gaming_key',
    resave: false,
    saveUninitialized: true
}));

// Authentication Middleware (The 'Security Guard')
const requireAuth = (req, res, next) => {
    if (req.session.loggedIn) {
        next(); // User has the keycard, let them in
    } else {
        res.redirect('/login'); // No keycard, send back to login
    }
};

// Root route now tries to go to dashboard
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
        req.session.loggedIn = true; // Give the user a session keycard
        res.redirect('/dashboard');
    } else {
        res.send('Access Denied: Incorrect password.');
    }
});

// Protected dashboard route (Only accessible if requireAuth passes)
app.get('/dashboard', requireAuth, (req, res) => {
    res.render('dashboard');
});

// Start the server
app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});