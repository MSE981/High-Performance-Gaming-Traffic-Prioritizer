import os
import pathlib

repo_root = pathlib.Path(__file__).resolve().parent.parent.parent
views_dir = repo_root / 'server' / 'views'
app_js = repo_root / 'server' / 'app.js'

# Update app.js
with open(app_js, 'r', encoding='utf-8') as f:
    app_code = f.read()

app_code = app_code.replace("'/more'", "'/security'")
app_code = app_code.replace('"/more"', '"/security"')
app_code = app_code.replace("render('more'", "render('security'")

with open(app_js, 'w', encoding='utf-8') as f:
    f.write(app_code)

# Rename more.ejs to security.ejs
old_more = views_dir / 'more.ejs'
new_security = views_dir / 'security.ejs'
if old_more.exists():
    old_more.rename(new_security)

# Update all views
for file in os.listdir(views_dir):
    if not file.endswith('.ejs'):
        continue
    filepath = views_dir / file
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Replace navbar links
    content = content.replace('<a href="/more"> More</a>', '<a href="/security"> Security</a>')
    content = content.replace('<a href="/more" class="active"> More</a>', '<a href="/security" class="active"> Security</a>')

    if file == 'security.ejs':
        content = content.replace('<title>More Features</title>', '<title>Security & Password</title>')

    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)

print('Refactor successful')
