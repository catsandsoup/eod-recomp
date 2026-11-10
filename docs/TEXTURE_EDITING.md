# Editing textures

Select a supported indexed texture and choose Export PNG. Edit the PNG in an
image editor, keeping its dimensions, then use Replace PNG. Save & Build writes
a separate mod overlay. Run Mod in Game starts a test session with that overlay.

Model previews show pending and saved texture edits. For the Bus default test
paint, editing Bus1 or Bus2 also updates the corresponding race-paint textures
when saved. Changing the Bus template again replaces those linked paint edits.
This applies to the default Bus test paint; arbitrary livery selection is not
implemented. Other vehicle-template remaps are not assumed.

Native imports keep the original indexed palette limits and regenerate existing
mip levels. 4K replacement textures, PBR and HDR materials are not supported.
