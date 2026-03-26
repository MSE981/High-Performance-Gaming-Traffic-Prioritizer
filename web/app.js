const express = require('express');
const path = require('path');
const app = express();

const PORT = 5000;

// Configure EJS as the template engine
app.set('view engine', 'ejs');
app.set('views', path.join(__dirname, 'views'));

// Middleware to parse form data from the login page
app.use(express.urlencoded({ extended: true }));

// Redirect root URL to the login page
app.get('/', (req, res) => {
    res.redirect('/login');
});

// Serve the login page
app.get('/login', (req, res) => {
    res.render('login');
});

// Handle the login form submission
app.post('/api/login', (req, res) => {
    const { password } = req.body;

    // Simple password check matching the original logic
    if (password === 'admin123') {
        res.send('Login successful! Dashboard coming soon.');
    } else {
        res.send('Access Denied: Incorrect password.');
    }
});

// Start the server
app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});