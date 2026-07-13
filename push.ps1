$repoUrl = "https://github.com/PaekHyun/TodayGoogleCalendar.git"
Set-Location "D:\SEMCOWork\Session18_esp32-google-calendar\xiao_esp32c6_epaper_calendar"

# Add remote
git remote add origin $repoUrl 2>$null
if ($LASTEXITCODE -ne 0) {
    git remote set-url origin $repoUrl
}

# Add all files
git add -A

# Commit
git commit -m "Initial commit: Google Calendar e-paper display for XIAO ESP32C6"

# Push
git branch -M main
git push -u origin main

Write-Host "Done!"
