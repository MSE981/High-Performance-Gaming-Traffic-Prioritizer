import os
import re

views_dir = r"c:\Users\Andrea\Desktop\REAL\High-Performance-Gaming-Traffic-Prioritizer\web\views"
app_js = r"c:\Users\Andrea\Desktop\REAL\High-Performance-Gaming-Traffic-Prioritizer\web\app.js"

# Update app.js
with open(app_js, 'r', encoding='utf-8') as f:
    app_code = f.read()

app_code = app_code.replace("'/more'", "'/security'")
app_code = app_code.replace('"/more"', '"/security"')
app_code = app_code.replace("render('more'", "render('security'")

with open(app_js, 'w', encoding='utf-8') as f:
    f.write(app_code)

# Rename more.ejs to security.ejs
old_more = os.path.join(views_dir, "more.ejs")
new_security = os.path.join(views_dir, "security.ejs")
if os.path.exists(old_more):
    os.rename(old_more, new_security)

# Update all views
for file in os.listdir(views_dir):
    if not file.endswith(".ejs"): continue
    filepath = os.path.join(views_dir, file)
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Replace navbar links
    content = content.replace('<a href="/more"> More</a>', '<a href="/security"> Security</a>')
    content = content.replace('<a href="/more" class="active"> More</a>', '<a href="/security" class="active"> Security</a>')
    
    if file == "security.ejs":
        content = content.replace('<title>More Features</title>', '<title>Security & Password</title>')
        # Remove the layout container sidebar since it's just one section now
        # Actually the user just said "不要more这个标题了直接就是security&password" meaning maybe in the nav.
        
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(content)

print('Refactor successful')
