# UltimateXR hand assets

The hand geometry and source mask textures used to build these runtime assets
come from [VRMADA/ultimatexr-unity](https://github.com/VRMADA/ultimatexr-unity),
commit `985f20c`.

Used source assets:

- `Runtime/Art/Avatars/BigHands/BigHandLeftGeo.fbx`
- `Runtime/Art/Avatars/BigHands/BigHandRightGeo.fbx`
- `Runtime/Art/Avatars/BigHands/HandPoses/Default.asset`
- `Runtime/Art/Avatars/BigHands/HandPoses/Grab.asset`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask1.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask2.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask3.png`

The FBX meshes are converted offline to the `UXRH` runtime format. The open,
grip, trigger and combined poses are baked from UltimateXR's authored finger
poses. `BigHandsAlbedo.rgba` is a raw 1024x1024 RGBA8 skin-colour
approximation made offline from the original masks; it does not include Unity
or Shader Graph code. Expected SHA-256:
`46BA8B2F08CE1CA420537E060F64BE139DD39A22357D4EA5B9B7B69DA2BA2AD4`.

`tools/hands_assets.py` embeds both meshes as stored and converts the albedo
into a 512x512 sRGB mip chain when the native library is built.

UltimateXR is distributed under the MIT License. See `ULTIMATEXR_LICENSE.txt`
in this directory.
