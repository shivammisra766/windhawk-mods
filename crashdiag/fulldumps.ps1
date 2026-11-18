$k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\explorer.exe'
New-Item -Path $k -Force | Out-Null
New-ItemProperty -Path $k -Name DumpFolder -PropertyType ExpandString -Value 'C:\Users\shiva\AppData\Local\CrashDumps\full' -Force | Out-Null
New-ItemProperty -Path $k -Name DumpType -PropertyType DWord -Value 2 -Force | Out-Null
New-ItemProperty -Path $k -Name DumpCount -PropertyType DWord -Value 3 -Force | Out-Null
Get-ItemProperty $k | Out-File 'C:\Users\shiva\AppData\Local\CrashDumps\full\config.txt'
