package com.theawesomeray.vespercine

import androidx.core.content.FileProvider

// Own subclass so the manifest entry can never clash with a FileProvider declared by a library.
class VesperFileProvider : FileProvider()
