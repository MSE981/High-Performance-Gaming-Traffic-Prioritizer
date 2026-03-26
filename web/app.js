const express = require('express');
const app = express();

// Set the server port to 5000, matching the original Python configuration
const PORT = 5000;

// Root route to test if the server is running correctly
app.get('/', (req, res) => {
    res.send('Raspberry Pi Node.js server successfully started!');
});

// Start the server and listen on the specified port
app.listen(PORT, () => {
    console.log(`Server is running at http://localhost:${PORT}`);
});