# Source Video Render

[Download](https://github.com/crashfort/SourceDemoRender/releases)

Fork of SourceDemoRender with a few tweaks so it integrates easier with demrec.

> Only works with TF2 x64, due to a cool hardcoded demotick address!

### Changes
- Process now stays open until game exists
- Modified process log to include hello/init messages & game exit code for detecting crashes
- Launch options are given through the command line instead of external file
- Process can be launched from anywhere now (before, CWD had to be in the same location as exe)
- Movie output directory can now be changed in profile config
- Added option to redirect velocity overlay to file
