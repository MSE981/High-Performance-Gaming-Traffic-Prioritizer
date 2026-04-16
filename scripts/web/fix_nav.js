const fs = require('fs');
const path = require('path');

const repoRoot = path.join(__dirname, '..', '..');
const views_dir = path.join(repoRoot, 'server', 'views');

const files = fs.readdirSync(views_dir);
for (const file of files) {
    if (!file.endsWith('.ejs')) continue;
    const filepath = path.join(views_dir, file);
    let content = fs.readFileSync(filepath, 'utf8');

    // Replace all instances of href="/more" logic
    content = content.replace(/<a href="\/more" class="active">.*?<\/a>/g, '<a href="/security" class="active"> Security</a>');
    content = content.replace(/<a href="\/more">.*?<\/a>/g, '<a href="/security"> Security</a>');

    // In case any were already mistakenly half-replaced in the previous script
    content = content.replace(/<a href="\/security" class="active"> More<\/a>/g, '<a href="/security" class="active"> Security</a>');
    content = content.replace(/<a href="\/security"> More<\/a>/g, '<a href="/security"> Security</a>');

    fs.writeFileSync(filepath, content);
}
console.log('Fixed the nav links');
