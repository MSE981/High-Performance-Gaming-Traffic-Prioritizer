const fs = require('fs');
const path = require('path');

const views_dir = 'c:/Users/Andrea/Desktop/REAL/High-Performance-Gaming-Traffic-Prioritizer/web/views';

const files = fs.readdirSync(views_dir);
for (const file of files) {
    if (!file.endsWith('.ejs')) continue;
    const filepath = path.join(views_dir, file);
    let content = fs.readFileSync(filepath, 'utf8');
    
    // The exact string in dashboard.ejs is: <a href="/wifi"> Wi-Fi</a>
    // We should match taking whitespace/newlines into account around the anchor tag
    content = content.replace(/[ \t]*<a href="\/wifi".*?>.*?Wi-Fi<\/a>\r?\n?/g, '');
    
    fs.writeFileSync(filepath, content);
}

const wifi_file = path.join(views_dir, 'wifi.ejs');
if (fs.existsSync(wifi_file)) fs.unlinkSync(wifi_file);

console.log('Removed wifi navigation and file');
