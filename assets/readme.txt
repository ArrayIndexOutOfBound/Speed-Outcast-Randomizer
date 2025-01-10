Assets files are now stored in this assets folder to make them easier to edit (no more manually picking files out of .pk3s for me!)

I will try to make sure assets_randomizer.pk3 in this folder is up to date, if you want to regenerate the file just run BundleAssets.ps1 and it will create a new assets_randomizer.pk3 in this folder with the latest files from models & ui.

If you want to add a new folder to the pk3 then update the PATH argument in BundleAssets.ps1 to include the new folder.

//TODO: Look at creating a github action that auto-generates this file for releases