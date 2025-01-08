if(Get-Item assets_randomizer.pk3)
{
    Remove-Item assets_randomizer.pk3
}

$compress = @{
  Path = "ui", "models"
  CompressionLevel = "Fastest"
  DestinationPath = "assets_randomizer.zip"
}
Compress-Archive @compress

Move-Item assets_randomizer.zip assets_randomizer.pk3