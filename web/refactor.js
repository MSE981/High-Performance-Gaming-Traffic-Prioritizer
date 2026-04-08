const fs = require('fs');
const path = require('path');

const views_dir = 'c:/Users/Andrea/Desktop/REAL/High-Performance-Gaming-Traffic-Prioritizer/web/views';
const app_js = 'c:/Users/Andrea/Desktop/REAL/High-Performance-Gaming-Traffic-Prioritizer/web/app.js';

let app_code = fs.readFileSync(app_js, 'utf8');
app_code = app_code.replace(/\/more/g, '/security');
app_code = app_code.replace(/render\('more'/g, "render('security'");
fs.writeFileSync(app_js, app_code);

const old_more = path.join(views_dir, 'more.ejs');
const new_security = path.join(views_dir, 'security.ejs');
if (fs.existsSync(old_more)) {
    fs.renameSync(old_more, new_security);
}

const files = fs.readdirSync(views_dir);
for (const file of files) {
    if (!file.endsWith('.ejs')) continue;
    const filepath = path.join(views_dir, file);
    let content = fs.readFileSync(filepath, 'utf8');
    
    content = content.replace(/<a href="\/security"> More<\/a>/g, '<a href="/security"> Security</a>');
    content = content.replace(/<a href="\/security" class="active"> More<\/a>/g, '<a href="/security" class="active"> Security</a>');
    
    // Oh wait, in app.js replace above, didn't do views files yet, so they still have /more.
    content = content.replace(/<a href="\/more"> More<\/a>/g, '<a href="/security"> Security</a>');
    content = content.replace(/<a href="\/more" class="active"> More<\/a>/g, '<a href="/security" class="active"> Security</a>');
    
    if (file === 'security.ejs' || file === 'more.ejs') {
        content = content.replace(/<title>More Features<\/title>/g, '<title>Security & Password</title>');
    }
    
    fs.writeFileSync(filepath, content);
}

console.log('Refactor successful');
