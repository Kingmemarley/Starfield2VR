# Port report: Starfield 1.16.236.0 -> 1.16.244.0

450 of 450 places ported. Calls not understood: 0. Checked runs not as in 1.16.236.0: 0.

## Not converted to GameCode yet (2; in this build their features read 1.16.236's places)
- CreationEngine\CreationEngineWeaponModule.cpp:10: 'MemoryScan::mod + 0x37de714': the game's base added to a place by hand (not converted yet)
- CreationEngine\CreationEngineWeaponModule.cpp:20: 'MemoryScan::mod + 0x26c2b40': the game's base added to a place by hand (not converted yet)

## CreationEngine\Climbing.cpp: 36 of 36
- 0x2b62b0 -> 0x2b5f30: (no unwind entry: code 0x2b62b0-0x2b62b4 between int3 paddings) the nearest of 2 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x47d9e0 -> 0x47d200: (no unwind entry: code 0x47d9e0-0x47d9e6 between int3 paddings) the nearest of 4 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x4951d0 -> 0x494960: the one function alike (one of its kind in both), 0x0 in
- 0x4952a6 -> 0x494a36: the one function alike (one of its kind in both), 0xd6 in
- 0x6634c0 -> 0x6627b0: the one function alike (one of its kind in both), 0x0 in
- 0xb05b80 -> 0xb055f0: the one function alike (one of its kind in both), 0x0 in
- 0xb05c0a -> 0xb0567a: the one function alike (one of its kind in both), 0x8a in
- 0xb05c17 -> 0xb05687: the one function alike (one of its kind in both), 0x97 in
- 0x18c5440 -> 0x18c2c30: the one function alike (one of its kind in both), 0x0 in
- 0x18c55a8 -> 0x18c2d98: the one function alike (one of its kind in both), 0x168 in
- 0x18c5601 -> 0x18c2df1: the one function alike (one of its kind in both), 0x1c1 in
- 0x18cf6c0 -> 0x18cceb0: (no unwind entry: code 0x18cf6c0-0x18cf6c9 between int3 paddings) 679 functions alike, none clearly the one by its body; by its place: number 3 of the 3 functions between 0x18cf660 and 0x18cf6d0 (each matched for certain), all alike and as many in both; its references fit it (1), 0x0 in
- 0x18cf6c4 -> 0x18cceb4: (no unwind entry: code 0x18cf6c0-0x18cf6c9 between int3 paddings) 679 functions alike, none clearly the one by its body; by its place: number 3 of the 3 functions between 0x18cf660 and 0x18cf6d0 (each matched for certain), all alike and as many in both; its references fit it (1), 0x4 in
- 0x18cfd40 -> 0x18cd530: the one function alike (one of its kind in both), 0x0 in
- 0x18cfd69 -> 0x18cd559: the one function alike (one of its kind in both), 0x29 in
- 0x193dcb0 -> 0x193b410: the one function alike (one of its kind in both), 0x0 in
- 0x193dd45 -> 0x193b4a5: the one function alike (one of its kind in both), 0x95 in
- 0x193de22 -> 0x193b582: the one function alike (one of its kind in both), 0x172 in
- 0x1947fb9 -> 0x1945719: the one function alike (one of its kind in both), 0x99 in
- 0x1a058dc -> 0x1a02e6c: the one function alike (one of its kind in both), 0x5c in
- 0x2358360 -> 0x2355d70: the one function alike (one of its kind in both), 0x0 in
- 0x2358456 -> 0x2355e66: the one function alike (one of its kind in both), 0xf6 in
- 0x235845b -> 0x2355e6b: the one function alike (one of its kind in both), 0xfb in
- 0x235846e -> 0x2355e7e: the one function alike (one of its kind in both), 0x10e in
- 0x235866b -> 0x235607b: the one function alike (one of its kind in both), 0x30b in
- 0x23586c6 -> 0x23560d6: the one function alike (one of its kind in both), 0x366 in
- 0x23586f3 -> 0x2356103: the one function alike (one of its kind in both), 0x393 in
- 0x23afe00 -> 0x23ad7f0: the one function alike (one of its kind in both), 0x40 in
- 0x23f4307 -> 0x23f1cf7: the one function alike (one of its kind in both), 0x18d in
- 0x23f45f6 -> 0x23f1fe6: the one function alike (one of its kind in both), 0x14f in
- 0x23f465c -> 0x23f204c: the one function alike (one of its kind in both), 0x1b5 in
- 0x2e93470 -> 0x2e8f770: (no unwind entry: code 0x2e93470-0x2e93483 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x2e934a0 -> 0x2e8f7a0: (no unwind entry: code 0x2e934a0-0x2e934c2 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x4cb9958 -> 0x4cb57b8: slot 0 of the type table of .?AV?$TypeErasedFuncImpl@V<lambda_b0651812a17537ef4a7a2b5bd049f263>@@XAEAVNiAVObject@@@detail@unique_function_impl@stl@@: the one table of that type in each exe (0x4cb9958, 0x4cb57b8), 5 slots in both, holding 0x6200a0, the port of 0x620c90 (matched for certain); the other rules agree
- 0x4cc2868 -> 0x4cbc448: slot 0 of the type table of .?AVPlayerCharacter@@: the one table of that type in each exe (0x4cc2868, 0x4cbc448), 421 slots in both, holding 0x1a1e9e0, the port of 0x1a21430 (matched for certain); the other rules agree
- 0x4d6d7d8 -> 0x4d697b0: slot 0 of the type table of .?AVbhkCharacterStateInAir@@: the one table of that type in each exe (0x4d6d7d8, 0x4d697b0), 9 slots in both; the other rules agree

## CreationEngine\Consume.cpp: 19 of 19
- 0xb48040 -> 0xb47920: the one function alike (one of its kind in both), 0x0 in
- 0x18cb1df -> 0x18c89cf: the one function alike (one of its kind in both), 0x3f in
- 0x18cb1e5 -> 0x18c89d5: the one function alike (one of its kind in both), 0x45 in
- 0x1a399c1 -> 0x1a36f71: the one function alike (one of its kind in both), 0x6d1 in
- 0x1a399eb -> 0x1a36f9b: the one function alike (one of its kind in both), 0x6fb in
- 0x1a39a0f -> 0x1a36fbf: the one function alike (one of its kind in both), 0x71f in
- 0x1a39a1c -> 0x1a36fcc: the one function alike (one of its kind in both), 0x72c in
- 0x1a39a20 -> 0x1a36fd0: the one function alike (one of its kind in both), 0x730 in
- 0x1a39a2e -> 0x1a36fde: the one function alike (one of its kind in both), 0x73e in
- 0x1a39a51 -> 0x1a37001: the one function alike (one of its kind in both), 0x761 in
- 0x1fc8e8f -> 0x1fc581f: the one function alike (one of its kind in both), 0x9f in
- 0x1fc8e95 -> 0x1fc5825: the one function alike (one of its kind in both), 0xa5 in
- 0x1fc8ec0 -> 0x1fc5850: the one function alike (one of its kind in both), 0xd0 in
- 0x1fc8ed8 -> 0x1fc5868: the one function alike (one of its kind in both), 0xe8 in
- 0x1fc8edf -> 0x1fc586f: the one function alike (one of its kind in both), 0xef in
- 0x1fc8f03 -> 0x1fc5893: the one function alike (one of its kind in both), 0x113 in
- 0x1fce980 -> 0x1fcb310: the one function alike (one of its kind in both), 0x0 in
- 0x1fce9b8 -> 0x1fcb348: the one function alike (one of its kind in both), 0x38 in
- 0x206a97d -> 0x20677dd: the one function alike (one of its kind in both), 0xbd in

## CreationEngine\ConversationPanel.cpp: 1 of 1
- 0x61ea0c0 -> 0x61e22b0: referred to at 0xb48040+0x306 (the one function alike (one of its kind in both)); 2 more reference(s) to it agree

## CreationEngine\DlssViews.cpp: 3 of 3
- 0x2923d40 -> 0x291fa90: the one function alike (one of its kind in both), 0x0 in
- 0x2ab6c30 -> 0x2ab2950: the one function alike (one of its kind in both), 0x0 in
- 0x5e99928 -> 0x5e91b68: referred to at 0x542f0+0xe (the nearest of 675 functions alike (as many in both) to where its neighbours moved)

## CreationEngine\EyeShare.cpp: 8 of 8
- 0x2b316a0 -> 0x2b2d3b0: the one function alike (one of its kind in both), 0x0 in
- 0x2b40720 -> 0x2b3c430: (no unwind entry: code 0x2b40720-0x2b40743 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x2b40730 -> 0x2b3c440: (no unwind entry: code 0x2b40720-0x2b40743 between int3 paddings) the one function alike (one of its kind in both), 0x10 in
- 0x2b40750 -> 0x2b3c460: the one function alike (one of its kind in both), 0x0 in
- 0x4ee2eb8 -> 0x4edee20: slot 0 of the type table of .?AVPrecomputeAtmosphericScatteringTransmittanceRenderPass@CreationRendererPrivate@@: the one table of that type in each exe (0x4ee2eb8, 0x4edee20), 8 slots in both; the other rules agree
- 0x4ee4498 -> 0x4ee0448: slot 0 of the type table of .?AVBlurTerrainHeightClipmapVerticalRenderPass@CreationRendererPrivate@@: the one table of that type in each exe (0x4ee4498, 0x4ee0448), 8 slots in both; the other rules agree
- 0x4ee44e0 -> 0x4ee0490: slot 0 of the type table of .?AVGenerateFogMapRenderPass@CreationRendererPrivate@@: the one table of that type in each exe (0x4ee44e0, 0x4ee0490), 8 slots in both; the other rules agree
- 0x4ee4528 -> 0x4ee0400: slot 0 of the type table of .?AVBlurTerrainHeightClipmapHorizontalRenderPass@CreationRendererPrivate@@: the one table of that type in each exe (0x4ee4528, 0x4ee0400), 8 slots in both; the other rules agree

## CreationEngine\FavoritesWheel.cpp: 36 of 36
- 0x5e49e0 -> 0x5e3e30: the one function alike (one of its kind in both), 0x0 in
- 0x63eef8 -> 0x63e2e8: the one function alike (one of its kind in both), 0x1a8 in
- 0x63ef1c -> 0x63e30c: the one function alike (one of its kind in both), 0x1cc in
- 0x641b30 -> 0x640f20: the one function alike (one of its kind in both), 0x0 in
- 0x641bb1 -> 0x640fa1: the one function alike (one of its kind in both), 0x81 in
- 0x64b3d0 -> 0x64a770: the one function alike (one of its kind in both), 0x0 in
- 0x900d20 -> 0x900490: (no unwind entry: code 0x900d20-0x900d91 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x19a6490 -> 0x19a3ba0: the one function alike (one of its kind in both), 0x0 in
- 0x1ebf53a -> 0x1ebc87a: the one function alike (one of its kind in both), 0x1da in
- 0x1ebf844 -> 0x1ebcb84: the one function alike (one of its kind in both), 0x224 in
- 0x1ebf864 -> 0x1ebcba4: the one function alike (one of its kind in both), 0x244 in
- 0x1ebf86b -> 0x1ebcbab: the one function alike (one of its kind in both), 0x24b in
- 0x1ec0b5c -> 0x1ebde9c: the one function alike (one of its kind in both), 0x1c in
- 0x1ec0b7a -> 0x1ebdeba: the one function alike (one of its kind in both), 0x3a in
- 0x1ec0bea -> 0x1ebdf2a: the one function alike (one of its kind in both), 0xaa in
- 0x1ec0bf1 -> 0x1ebdf31: the one function alike (one of its kind in both), 0xb1 in
- 0x1ec2040 -> 0x1ebf380: by hand (manual_1_16_244.txt line 39: its start: the same 31 bytes (push rbp .. mov r13,rcx))
- 0x1ec209d -> 0x1ebf3dd: by hand (manual_1_16_244.txt line 40: +0x5D: the slot read (r13+440h, +438h) and the handle's test, the same 37 bytes (the je at the same distance))
- 0x1ec4f10 -> 0x1ec2250: 2 functions alike, none clearly the one by its body; by its place: number 2 of the 2 functions between 0x1ec4d50 and 0x1ec4f30 (each matched for certain), all alike and as many in both; its references fit it (4) and not its 1 twin(s) between them, 0xb0 in
- 0x1ec4f17 -> 0x1ec2257: 2 functions alike, none clearly the one by its body; by its place: number 2 of the 2 functions between 0x1ec4d50 and 0x1ec4f30 (each matched for certain), all alike and as many in both; its references fit it (4) and not its 1 twin(s) between them, 0xb7 in
- 0x1ec4f1a -> 0x1ec225a: 2 functions alike, none clearly the one by its body; by its place: number 2 of the 2 functions between 0x1ec4d50 and 0x1ec4f30 (each matched for certain), all alike and as many in both; its references fit it (4) and not its 1 twin(s) between them, 0xba in
- 0x1ec54ca -> 0x1ec280a: the one function alike (one of its kind in both), 0xfa in
- 0x1ec54d1 -> 0x1ec2811: the one function alike (one of its kind in both), 0x101 in
- 0x1ec54d9 -> 0x1ec2819: the one function alike (one of its kind in both), 0x109 in
- 0x1ec54f0 -> 0x1ec2830: the one function alike (one of its kind in both), 0x120 in
- 0x1ec5501 -> 0x1ec2841: the one function alike (one of its kind in both), 0x131 in
- 0x1ec5506 -> 0x1ec2846: the one function alike (one of its kind in both), 0x136 in
- 0x1ec550f -> 0x1ec284f: the one function alike (one of its kind in both), 0x13f in
- 0x22cf380 -> 0x22ccd40: (no unwind entry: code 0x22cf380-0x22cf415 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x3a442c8 -> 0x3a402c8: referred to at 0x2c5e90+0x1c9 (the one function alike (one of its kind in both)); 68 more reference(s) to it agree
- 0x4b07de8 -> 0x4b03d40: slot 0 of the type table of .?AVIDataModel@@: the one table of that type in each exe (0x4b07de8, 0x4b03d40), 4 slots in both; the other rules agree
- 0x4d0d398 -> 0x4d094e0: slot 0 of the type table of .?AVFavoritesManager@@: the one table of that type in each exe (0x4d0d398, 0x4d094e0), 4 slots in both, holding 0x18a0070, the port of 0x18a2830 (matched for certain); the other rules agree
- 0x5f5e9d8 -> 0x5f56c58: referred to at 0x1ec53d0+0x101 (the one function alike (one of its kind in both))
- 0x61e5258 -> 0x61dd4c8: referred to at 0x1e97f40+0x24d (the one function alike (one of its kind in both)); 2 more reference(s) to it agree
- 0x61ea358 -> 0x61e2540: referred to at 0xc3bb30+0x570 (the one function alike (one of its kind in both))
- 0x61f6330 -> 0x61ee4f8: referred to at 0x1ec0b40+0x3a (the one function alike (one of its kind in both)); 1 more reference(s) to it agree

## CreationEngine\Foveation.cpp: 2 of 2
- 0x2b59040 -> 0x2b54d30: the one function alike (one of its kind in both), 0x0 in
- 0x5e7e31c -> 0x5e7655c: referred to at 0x2b59040+0x1a1 (the one function alike (one of its kind in both))

## CreationEngine\GloveHighlight.cpp: 45 of 45
- 0x3e69b5 -> 0x3e61c5: the one function alike (one of its kind in both), 0x65 in
- 0x3e69cf -> 0x3e61df: the one function alike (one of its kind in both), 0x7f in
- 0x3e6a4b -> 0x3e625b: the one function alike (one of its kind in both), 0xfb in
- 0x3efc10 -> 0x3ef420: (no unwind entry: code 0x3efc10-0x3efc19 between int3 paddings) the nearest of 36 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x3efc46 -> 0x3ef456: (no unwind entry: code 0x3efc20-0x3efc4d between int3 paddings) the one function alike (one of its kind in both), 0x26 in
- 0x3efc4c -> 0x3ef45c: (no unwind entry: code 0x3efc20-0x3efc4d between int3 paddings) the one function alike (one of its kind in both), 0x2c in
- 0x3fa780 -> 0x3f9f90: (no unwind entry: code 0x3fa780-0x3fa789 between int3 paddings) 36 functions alike, none clearly the one by its body; by its place: number 2 of the 2 functions between 0x3fa5b0 and 0x3fa790 (each matched for certain), all alike and as many in both; its references fit it (1), 0x0 in
- 0x3fa7fb -> 0x3fa00b: the one function alike (one of its kind in both), 0x6b in
- 0x3fa800 -> 0x3fa010: the one function alike (one of its kind in both), 0x70 in
- 0x5def30 -> 0x5de3b0: the one function alike (one of its kind in both), 0x0 in
- 0x653fc0 -> 0x6532f0: the one function alike (one of its kind in both), 0x0 in
- 0x653fdc -> 0x65330c: the one function alike (one of its kind in both), 0x1c in
- 0x65409d -> 0x6533cd: the one function alike (one of its kind in both), 0xdd in
- 0x6540b1 -> 0x6533e1: the one function alike (one of its kind in both), 0xf1 in
- 0x654160 -> 0x653490: the one function alike (one of its kind in both), 0x0 in
- 0x654520 -> 0x653850: the one function alike (one of its kind in both), 0x0 in
- 0x7cbb50 -> 0x7cb120: the one function alike (one of its kind in both), 0x0 in
- 0x7cbb6b -> 0x7cb13b: the one function alike (one of its kind in both), 0x1b in
- 0xb790c0 -> 0xb78980: the one function alike (one of its kind in both), 0x0 in
- 0x17d775f -> 0x17d4dff: by hand (manual_1_16_244.txt line 105: lea rax,[the mark visitor] ... call the walk (0x24181E0): the scanner marks an object, as in 1.16.236)
- 0x17d777d -> 0x17d4e1d: by hand (manual_1_16_244.txt line 106: call the walk (0x24181E0, the walk's port): the same call, 0x1E after the lea in both)
- 0x1a953d0 -> 0x1a92990: the one function alike (one of its kind in both), 0x0 in
- 0x1a954b5 -> 0x1a92a75: the one function alike (one of its kind in both), 0xe5 in
- 0x1a954be -> 0x1a92a7e: the one function alike (one of its kind in both), 0xee in
- 0x2017963 -> 0x20144e3: (no unwind entry: code 0x2017960-0x2017968 between int3 paddings) 2 functions alike, none clearly the one by its body; by its place: number 4 of the 6 functions between 0x2017890 and 0x2017990 (each matched for certain), all alike and as many in both; its references fit it (1), 0x3 in
- 0x20497b9 -> 0x20464a9: the one function alike (one of its kind in both), 0xe9 in
- 0x2049845 -> 0x2046535: (no unwind entry: code 0x2049840-0x2049863 between int3 paddings) the one function alike (one of its kind in both), 0x5 in
- 0x204984f -> 0x204653f: (no unwind entry: code 0x2049840-0x2049863 between int3 paddings) the one function alike (one of its kind in both), 0xf in
- 0x241a660 -> 0x24181e0: the one function alike (one of its kind in both), 0x0 in
- 0x243fbf3 -> 0x243d973: the one function alike (one of its kind in both), 0x43 in
- 0x243fbfc -> 0x243d97c: the one function alike (one of its kind in both), 0x4c in
- 0x24c1a01 -> 0x24bf7d1: the one function alike (one of its kind in both), 0x71 in
- 0x24c1a0d -> 0x24bf7dd: the one function alike (one of its kind in both), 0x7d in
- 0x4b0def0 -> 0x4b09ef0: slot 0 of the type table of .?AVEffectSequenceMap@@: the one table of that type in each exe (0x4b0def0, 0x4b09ef0), 3 slots in both, holding 0x3ec940, the port of 0x3ed130 (matched for certain); the other rules agree
- 0x4b0e248 -> 0x4b0a248: slot 0 of the type table of .?AVTrackGroup@BSSequence@@: the one table of that type in each exe (0x4b0e248, 0x4b0a248), 9 slots in both; the other rules agree
- 0x4b339d8 -> 0x4b2fa50: slot 0 of the type table of .?AVHighlightManager@@: the one table of that type in each exe (0x4b339d8, 0x4b2fa50), 1 slots in both, holding 0x653400, the port of 0x6540d0 (matched for certain); the other rules agree
- 0x4b33a48 -> 0x4b2fa10: slot 0 of the type table of .?AV?$LocalFunctionImpl@V<lambda_c2d0a227a1700518fcc225232cb794ab>@@W4BSVisitControl@BSVisit@@AEAVNiAVObject@@@local_function_detail@stl@@: the one table of that type in each exe (0x4b33a48, 0x4b2fa10), 3 slots in both; the other rules agree
- 0x4b533c8 -> 0x4b4f3b0: slot 0 of the type table of .?AVBGSEffectSequenceForm@@: the one table of that type in each exe (0x4b533c8, 0x4b4f3b0), 108 slots in both, holding 0x86c6f0, the port of 0x86cf50 (matched for certain); the other rules agree
- 0x4b5c148 -> 0x4b57980: slot 0 of the type table of .?AVParticleEffectTrack@BSSequence@@: the one table of that type in each exe (0x4b5c148, 0x4b57980), 9 slots in both; the other rules agree
- 0x4b5c278 -> 0x4b579d0: slot 0 of the type table of .?AVParticleEvent@BSSequence@@: the one table of that type in each exe (0x4b5c278, 0x4b579d0), 9 slots in both; the other rules agree
- 0x4b5cdb8 -> 0x4b58740: slot 0 of the type table of .?AVLightEffectTrack@BSSequence@@: the one table of that type in each exe (0x4b5cdb8, 0x4b58740), 10 slots in both; the other rules agree
- 0x4b5cee0 -> 0x4b58798: slot 0 of the type table of .?AVLightSpawnEvent@BSSequence@@: the one table of that type in each exe (0x4b5cee0, 0x4b58798), 9 slots in both; the other rules agree
- 0x4b823d8 -> 0x4b7e440: slot 0 of the type table of .?AVTESEffectShader@@: the one table of that type in each exe (0x4b823d8, 0x4b7e440), 98 slots in both, holding 0xa56d20, the port of 0xa57410 (matched for certain); the other rules agree
- 0x597d1f8 -> 0x59751e8: referred to at 0x654520+0x18 (the one function alike (one of its kind in both)); 1 more reference(s) to it agree
- 0x5fe1908 -> 0x5fd9b88: referred to at 0xb48040+0x25b (the one function alike (one of its kind in both)); 1 more reference(s) to it agree

## CreationEngine\GrenadeHand.cpp: 26 of 26
- 0x2c4160 -> 0x2c3ba0: the one function alike (one of its kind in both), 0x0 in
- 0x3537c0 -> 0x353250: the one function alike (one of its kind in both), 0x0 in
- 0x5a10e0 -> 0x5a0520: the one function alike (one of its kind in both), 0x0 in
- 0x5a1130 -> 0x5a0570: the one function alike (one of its kind in both), 0x50 in
- 0x5a27dd -> 0x5a1c1d: the one function alike (one of its kind in both), 0x16fd in
- 0xb25c70 -> 0xb25520: the one function alike (one of its kind in both), 0x0 in
- 0xb52189 -> 0xb51a69: the one function alike (one of its kind in both), 0x969 in
- 0x183ed20 -> 0x183c590: the one function alike (one of its kind in both), 0x0 in
- 0x1852ac7 -> 0x18501c4: by hand (manual_1_16_244.txt line 10: mov r14, [the manager] (1.16.236: mov r15, ...); RipTarget(at, 7) is the manager in both)
- 0x1852ace -> 0x18501d3: by hand (manual_1_16_244.txt line 11: cmp dword [r14+50h], 0 (1.16.236: [r15+50h], 7 bytes after the mov; here 15))
- 0x1853260 -> 0x1850940: the one function alike (one of its kind in both), 0x0 in
- 0x187e370 -> 0x187bb60: the one function alike (one of its kind in both), 0x0 in
- 0x2066f7c -> 0x2063ddc: the one function alike (one of its kind in both), 0x3bc in
- 0x2068840 -> 0x20656a0: the one function alike (one of its kind in both), 0x0 in
- 0x20688dd -> 0x206573d: the one function alike (one of its kind in both), 0x9d in
- 0x2068950 -> 0x20657b0: the one function alike (one of its kind in both), 0x110 in
- 0x2068a00 -> 0x2065860: the one function alike (one of its kind in both), 0x0 in
- 0x207bcc0 -> 0x2078b20: the one function alike (one of its kind in both), 0x0 in
- 0x207c094 -> 0x2078ef4: the one function alike (one of its kind in both), 0x3d4 in
- 0x207c110 -> 0x2078f70: the one function alike (one of its kind in both), 0x450 in
- 0x207c205 -> 0x2079065: the one function alike (one of its kind in both), 0x545 in
- 0x207d2db -> 0x207a13b: the one function alike (one of its kind in both), 0x32b in
- 0x207d32f -> 0x207a18f: the one function alike (one of its kind in both), 0x37f in
- 0x2083837 -> 0x2080697: the one function alike (one of its kind in both), 0x97 in
- 0x2083877 -> 0x20806d7: the one function alike (one of its kind in both), 0xd7 in
- 0x5e754a8 -> 0x5e6d6e8: referred to at 0xc2d27d+0x7 (the one function alike (one of its kind in both)); 20 more reference(s) to it agree

## CreationEngine\HandGrab.cpp: 22 of 22
- 0x510d40 -> 0x5105c0: the one function alike (one of its kind in both), 0x0 in
- 0xd4e6e0 -> 0xd4e0d0: the one function alike (one of its kind in both), 0x0 in
- 0xd4e7b4 -> 0xd4e1a4: the one function alike (one of its kind in both), 0xd4 in
- 0xd4e7c9 -> 0xd4e1b9: the one function alike (one of its kind in both), 0xe9 in
- 0xd4e7de -> 0xd4e1ce: the one function alike (one of its kind in both), 0xfe in
- 0xd4e8e5 -> 0xd4e2d5: the one function alike (one of its kind in both), 0x205 in
- 0x22cea30 -> 0x22cc3d0: the one function alike (one of its kind in both), 0x0 in
- 0x2361d40 -> 0x235f750: the one function alike (one of its kind in both), 0x0 in
- 0x2361f50 -> 0x235f960: (no unwind entry: code 0x2361f50-0x2361fa7 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x237863a -> 0x237600a: by hand (manual_1_16_244.txt line 115: lea rdx,[rbp+0xC0] (the transform); mov rcx,r14 (the collision object); vzeroupper; call the writer (0x237A4F0); returns to 0x237601C)
- 0x23798a0 -> 0x2377270: the one function alike (one of its kind in both), 0x0 in
- 0x23799ef -> 0x23773bf: the one function alike (one of its kind in both), 0x14f in
- 0x2379a60 -> 0x2377430: the one function alike (one of its kind in both), 0x0 in
- 0x237cb20 -> 0x237a4f0: the one function alike (one of its kind in both), 0x0 in
- 0x238c170 -> 0x2389b40: the one function alike (one of its kind in both), 0x0 in
- 0x241a2e0 -> 0x2417e60: the one function alike (one of its kind in both), 0x0 in
- 0x2e934a0 -> 0x2e8f7a0: (no unwind entry: code 0x2e934a0-0x2e934c2 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x3060ec0 -> 0x305d1c0: the one function alike (one of its kind in both), 0x0 in
- 0x308b8c0 -> 0x3087bc0: the one function alike (one of its kind in both), 0x0 in
- 0x4cc19a0 -> 0x4cbef20: slot 0 of the type table of .?AV?$LocalFunctionImpl@V<lambda_09861f9863174d1d28bdf7929e559315>@@W4BSVisitControl@BSVisit@@AEAVNiAVObject@@@local_function_detail@stl@@: the one table of that type in each exe (0x4cc19a0, 0x4cbef20), 3 slots in both; the other rules agree
- 0x4d67ac0 -> 0x4d636b0: slot 0 of the type table of .?AVbhkNPCollisionProxyObject@@: the one table of that type in each exe (0x4d67ac0, 0x4d636b0), 60 slots in both, holding 0x237aee0, the port of 0x237d510 (matched for certain); the other rules agree
- 0x61e7488 -> 0x61df6e8: referred to at 0xb05b80+0x71 (the one function alike (one of its kind in both)); 7 more reference(s) to it agree

## CreationEngine\ImpactMelee.cpp: 10 of 10
- 0x19813d0 -> 0x197eb20: the one function alike (one of its kind in both), 0x0 in
- 0x19813f3 -> 0x197eb43: the one function alike (one of its kind in both), 0x23 in
- 0x1981560 -> 0x197ecb0: the one function alike (one of its kind in both), 0x190 in
- 0x1981944 -> 0x197f094: the one function alike (one of its kind in both), 0x574 in
- 0x19819eb -> 0x197f13b: the one function alike (one of its kind in both), 0x61b in
- 0x19819f0 -> 0x197f140: the one function alike (one of its kind in both), 0x620 in
- 0x1987b20 -> 0x1985270: the one function alike (one of its kind in both), 0x0 in
- 0x198c9e0 -> 0x198a0f0: the one function alike (one of its kind in both), 0x0 in
- 0x198ca84 -> 0x198a194: the one function alike (one of its kind in both), 0xa4 in
- 0x1b2e170 -> 0x1b2b7e0: the one function alike (one of its kind in both), 0x0 in

## CreationEngine\InputMode.cpp: 1 of 1
- 0x5fe1a50 -> 0x5fd9cd8: referred to at 0x1518650+0x336 (the one function alike (one of its kind in both)); 1 more reference(s) to it agree

## CreationEngine\LadderClimb.cpp: 7 of 7
- 0x1a390c0 -> 0x1a36670: the one function alike (one of its kind in both), 0x110 in
- 0x1e91bd0 -> 0x1e8f130: the one function alike (one of its kind in both), 0x0 in
- 0x1e98e1b -> 0x1e9637b: the one function alike (one of its kind in both), 0xedb in
- 0x1e9c5c0 -> 0x1e99b20: (no unwind entry: code 0x1e9c5c0-0x1e9c5cd between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x1e9c5d0 -> 0x1e99b30: (no unwind entry: code 0x1e9c5d0-0x1e9c5e6 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x4d09bb8 -> 0x4d05bc8: slot 0 of the type table of .?AVThirdPersonState@@: the one table of that type in each exe (0x4d09bb8, 0x4d05bc8), 26 slots in both; the other rules agree
- 0x61e51f0 -> 0x61dd460: referred to at 0xb48040+0x4ab (the one function alike (one of its kind in both)); 11 more reference(s) to it agree

## CreationEngine\LeftWeapon.cpp: 40 of 40
- 0x2c5e90 -> 0x2c5870: the one function alike (one of its kind in both), 0x0 in
- 0x3476e0 -> 0x347170: the one function alike (one of its kind in both), 0x0 in
- 0x347701 -> 0x347191: the one function alike (one of its kind in both), 0x21 in
- 0x4015e0 -> 0x400de0: the one function alike (one of its kind in both), 0x0 in
- 0x4015f4 -> 0x400df4: the one function alike (one of its kind in both), 0x14 in
- 0x401634 -> 0x400e34: the one function alike (one of its kind in both), 0x54 in
- 0x402080 -> 0x401880: the one function alike (one of its kind in both), 0x0 in
- 0x4020b2 -> 0x4018b2: the one function alike (one of its kind in both), 0x32 in
- 0x4020b6 -> 0x4018b6: the one function alike (one of its kind in both), 0x36 in
- 0x402c10 -> 0x402410: the one function alike (one of its kind in both), 0x0 in
- 0x402c6f -> 0x40246f: the one function alike (one of its kind in both), 0x5f in
- 0x402cb7 -> 0x4024b7: the one function alike (one of its kind in both), 0xa7 in
- 0x5def30 -> 0x5de3b0: the one function alike (one of its kind in both), 0x0 in
- 0x63e4c0 -> 0x63d8b0: the one function alike (one of its kind in both), 0x0 in
- 0x63ed50 -> 0x63e140: the one function alike (one of its kind in both), 0x0 in
- 0x63eec2 -> 0x63e2b2: the one function alike (one of its kind in both), 0x172 in
- 0x63eec9 -> 0x63e2b9: the one function alike (one of its kind in both), 0x179 in
- 0x63eece -> 0x63e2be: the one function alike (one of its kind in both), 0x17e in
- 0x63eef3 -> 0x63e2e3: the one function alike (one of its kind in both), 0x1a3 in
- 0x63eef8 -> 0x63e2e8: the one function alike (one of its kind in both), 0x1a8 in
- 0x63ef1c -> 0x63e30c: the one function alike (one of its kind in both), 0x1cc in
- 0x15e9090 -> 0x15e8080: by hand (manual_1_16_244.txt line 31: its start: the same 32 bytes (push rbp .. mov [rbp+50h],ebx))
- 0x15e90e9 -> 0x15e80d9: by hand (manual_1_16_244.txt line 32: +0x59: call the handle's form (0x2C5870, the port of 0x2C5E90), as in 1.16.236)
- 0x15e90ee -> 0x15e80de: by hand (manual_1_16_244.txt line 33: +0x5E: mov rsi,[rbp+58h] (the form found); test; je +0x1C4 (out); the same 25 bytes)
- 0x15e90fb -> 0x15e80eb: by hand (manual_1_16_244.txt line 34: +0x6B: the hook's place, mov word [rbp+48h],1 (the form found, nothing done with it yet; before the counter is raised))
- 0x15e9150 -> 0x15e8140: by hand (manual_1_16_244.txt line 35: +0xC0: call the equip (0x19A3330, the port of 0x19A5C20), as in 1.16.236)
- 0x15e92bc -> 0x15e82ac: by hand (manual_1_16_244.txt line 36: +0x22C: the skip's target, mov ebx,[rbp+50h] (after the counter is lowered; the handle let go after it))
- 0x19a5c20 -> 0x19a3330: the one function alike (one of its kind in both), 0x0 in
- 0x19a5c82 -> 0x19a3392: the one function alike (one of its kind in both), 0x62 in
- 0x19a5c89 -> 0x19a3399: the one function alike (one of its kind in both), 0x69 in
- 0x1ec2040 -> 0x1ebf380: by hand (manual_1_16_244.txt line 39: its start: the same 31 bytes (push rbp .. mov r13,rcx))
- 0x1ec209d -> 0x1ebf3dd: by hand (manual_1_16_244.txt line 40: +0x5D: the slot read (r13+440h, +438h) and the handle's test, the same 37 bytes (the je at the same distance))
- 0x1ec20b5 -> 0x1ebf3f5: by hand (manual_1_16_244.txt line 41: +0x75: the hook's place, xor dil,dil; cmp dword [rbp+67h],-1 (the handle known, nothing done with it yet))
- 0x1ec2336 -> 0x1ebf676: by hand (manual_1_16_244.txt line 42: +0x2F6: call the equip (0x19A3330, the port of 0x19A5C20), as in 1.16.236)
- 0x1ec24c7 -> 0x1ebf807: by hand (manual_1_16_244.txt line 43: +0x487: every way out: mov rcx,[the handles] (0x5E608C0, the port of 0x5E68680; 1.16.236's bytes there have its old distance); test; je; cmp [rbp+67h],-1; je; lea rdx,[rbp+67h]; call (let go))
- 0x2017963 -> 0x20144e3: (no unwind entry: code 0x2017960-0x2017968 between int3 paddings) 2 functions alike, none clearly the one by its body; by its place: number 4 of the 6 functions between 0x2017890 and 0x2017990 (each matched for certain), all alike and as many in both; its references fit it (1), 0x3 in
- 0x22ce9b0 -> 0x22cc350: the one function alike (one of its kind in both), 0x0 in
- 0x3a442c8 -> 0x3a402c8: referred to at 0x2c5e90+0x1c9 (the one function alike (one of its kind in both)); 68 more reference(s) to it agree
- 0x4b0e998 -> 0x4b0a998: slot 0 of the type table of .?AVBGSObjectInstanceExtra@@: the one table of that type in each exe (0x4b0e998, 0x4b0a998), 7 slots in both, holding 0x400de0, the port of 0x4015e0 (matched for certain); the other rules agree
- 0x5e68680 -> 0x5e608c0: referred to at 0xc3bb30+0xa91 (the one function alike (one of its kind in both)); 4 more reference(s) to it agree

## CreationEngine\LensFit.cpp: 4 of 4
- 0x29a5d55 -> 0x29a1ad5: the one function alike (one of its kind in both), 0x1c5 in
- 0x29a746c -> 0x29a31ec: the one function alike (one of its kind in both), 0x2c0 in
- 0x29bb4c1 -> 0x29b7241: the one function alike (one of its kind in both), 0x447 in
- 0x2b0c0f5 -> 0x2b07dd5: the one function alike (one of its kind in both), 0x2e1 in

## CreationEngine\MeleeThrow.cpp: 2 of 2
- 0x4cc41b0 -> 0x4cc01b0: slot 0 of the type table of .?AVProcessLists@@: the one table of that type in each exe (0x4cc41b0, 0x4cc01b0), 2 slots in both, holding 0x1a88fe0, the port of 0x1a8bad0 (matched for certain); the other rules agree
- 0x5fe1908 -> 0x5fd9b88: referred to at 0xb48040+0x25b (the one function alike (one of its kind in both)); 1 more reference(s) to it agree

## CreationEngine\MenuTaa.cpp: 3 of 3
- 0x840b67 -> 0x8402d7: the one function alike (one of its kind in both), 0x157 in
- 0x840b6c -> 0x8402dc: the one function alike (one of its kind in both), 0x15c in
- 0x840dc7 -> 0x840537: the one function alike (one of its kind in both), 0x3b7 in

## CreationEngine\MuzzleFire.cpp: 1 of 1
- 0x1b0a2ba -> 0x1b0797a: the one function alike (one of its kind in both), 0x81a in

## CreationEngine\Physical.cpp: 64 of 64
- 0x2b4ec0 -> 0x2b4b60: the one function alike (one of its kind in both), 0x0 in
- 0x2b8150 -> 0x2b7dd0: the nearest of 412 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x2d2330 -> 0x2d1be0: the one function alike (one of its kind in both), 0x0 in
- 0x384bd0 -> 0x384600: the one function alike (one of its kind in both), 0x0 in
- 0x4f0f20 -> 0x4f07a0: the one function alike (one of its kind in both), 0x0 in
- 0x6634c0 -> 0x6627b0: the one function alike (one of its kind in both), 0x0 in
- 0x12d6594 -> 0x12d5fb4: by hand (manual_1_16_244.txt line 79: mov rsi,[task queue]: 0x5E71728 in 1.16.244, the same global PushActorAway's code loads there (0x207DBEA), as in 1.16.236 (0x5E794E8))
- 0x12d6623 -> 0x12d6043: by hand (manual_1_16_244.txt line 80: mov r15d,0BCh; cmp byte [r12+r15],0; jne (the block's ready flag))
- 0x12d6630 -> 0x12d6050: by hand (manual_1_16_244.txt line 81: call 0x365D488: the block made ready (1.16.236 0x3661188; same bytes, tail jump to 0x365D420, the same TLS initializer))
- 0x12d6635 -> 0x12d6055: by hand (manual_1_16_244.txt line 82: mov eax,1D4h; mov ebx,[rax+r12] (the context saved))
- 0x12d6647 -> 0x12d6067: by hand (manual_1_16_244.txt line 83: call 0x2CC160: the context set (1.16.236 0x2CC8B0; the one function alike, writes +0x1D4))
- 0x12d6676 -> 0x12d6096: by hand (manual_1_16_244.txt line 84: call 0x22CDF60: the command taken (1.16.236 0x22D05A0; the one function alike))
- 0x12d667b -> 0x12d609b: by hand (manual_1_16_244.txt line 85: lea rcx,[run]; mov [rax+8],rcx: 0x18CC9E0, the tool's port of the run 0x18CF1F0)
- 0x12d6686 -> 0x12d60a6: by hand (manual_1_16_244.txt line 86: lea rcx,[type table]; mov [rax],rcx: 0x4CB5FA8, the same RTTI TCmd<lambda_e2add6adb287b8a88ba9ca51828c2086> as 0x4CBA048, its 3 slots alike)
- 0x12d66a0 -> 0x12d60c0: by hand (manual_1_16_244.txt line 87: call 0x34FE40: committed (1.16.236 0x3503B0; the one function alike))
- 0x18c19a0 -> 0x18bf190: the one function alike (one of its kind in both), 0x0 in
- 0x18c1a2d -> 0x18bf21d: the one function alike (one of its kind in both), 0x8d in
- 0x18c1a40 -> 0x18bf230: the one function alike (one of its kind in both), 0xa0 in
- 0x18c1b3e -> 0x18bf32e: the one function alike (one of its kind in both), 0x19e in
- 0x18c5440 -> 0x18c2c30: the one function alike (one of its kind in both), 0x0 in
- 0x18c55a8 -> 0x18c2d98: the one function alike (one of its kind in both), 0x168 in
- 0x18c5601 -> 0x18c2df1: the one function alike (one of its kind in both), 0x1c1 in
- 0x18caf63 -> 0x18c8753: the one function alike (one of its kind in both), 0x73 in
- 0x18cf1f0 -> 0x18cc9e0: (no unwind entry: code 0x18cf1f0-0x18cf1f9 between int3 paddings) 644 functions alike, none clearly the one by its body; by its place: number 9 of the 9 functions between 0x18cf140 and 0x18cf200 (each matched for certain), all alike and as many in both; its references fit it (1) and not its 3 twin(s) between them, 0x0 in
- 0x19038a0 -> 0x1901000: the one function alike (one of its kind in both), 0x0 in
- 0x194ca00 -> 0x194a160: (no unwind entry: code 0x194ca00-0x194ca1c between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x1983d89 -> 0x19814d9: the one function alike (one of its kind in both), 0x1819 in
- 0x1983d8e -> 0x19814de: the one function alike (one of its kind in both), 0x181e in
- 0x1996bb0 -> 0x19942c0: the one function alike (one of its kind in both), 0x0 in
- 0x1a051f0 -> 0x1a02780: the one function alike (one of its kind in both), 0x0 in
- 0x1a05300 -> 0x1a02890: the one function alike (one of its kind in both), 0x110 in
- 0x1a05308 -> 0x1a02898: the one function alike (one of its kind in both), 0x118 in
- 0x1a51b52 -> 0x1a4f142: by hand (manual_1_16_244.txt line 68: cmp [the setting's value],bl; jne (the same distance))
- 0x1aeee7e -> 0x1aec21e: the one function alike (one of its kind in both), 0x14e in
- 0x1aeeebf -> 0x1aec25f: the one function alike (one of its kind in both), 0x18f in
- 0x1aeeeca -> 0x1aec26a: the one function alike (one of its kind in both), 0x19a in
- 0x1aeeed8 -> 0x1aec278: the one function alike (one of its kind in both), 0x1a8 in
- 0x1aeeeef -> 0x1aec28f: the one function alike (one of its kind in both), 0x1bf in
- 0x1aeef10 -> 0x1aec2b0: the one function alike (one of its kind in both), 0x1e0 in
- 0x1fccde0 -> 0x1fc9770: the one function alike (one of its kind in both), 0x0 in
- 0x206e74a -> 0x206b5aa: the one function alike (one of its kind in both), 0x2aa in
- 0x206e74f -> 0x206b5af: the one function alike (one of its kind in both), 0x2af in
- 0x206e760 -> 0x206b5c0: the one function alike (one of its kind in both), 0x2c0 in
- 0x2080d8a -> 0x207dbea: the one function alike (one of its kind in both), 0x16a in
- 0x2080d91 -> 0x207dbf1: the one function alike (one of its kind in both), 0x171 in
- 0x2354360 -> 0x2351d70: the one function alike (one of its kind in both), 0x0 in
- 0x235444e -> 0x2351e5e: the one function alike (one of its kind in both), 0xee in
- 0x2379a60 -> 0x2377430: the one function alike (one of its kind in both), 0x0 in
- 0x28cef30 -> 0x28caea0: by hand (manual_1_16_244.txt line 55: push r12; sub rsp,40h; cmp qword [rcx],0; mov r12,rcx (1.16.236: r13))
- 0x3bbeac0 -> 0x3bbaac0: slot 0 of the type table of .?AVhknpCapsuleShape@@: the one table of that type in each exe (0x3bbeac0, 0x3bbaac0), 3 slots in both; the other rules agree
- 0x4afee08 -> 0x4afae08: slot 0 of the type table of .?AVBGSActionData@@ (its part at +0x28): the one table of that type in each exe (0x4afee08, 0x4afae08), 1 slots in both; the other rules agree
- 0x4afee18 -> 0x4afae18: slot 0 of the type table of .?AVBGSActionData@@: the one table of that type in each exe (0x4afee18, 0x4afae18), 2 slots in both; the other rules agree
- 0x4aff448 -> 0x4afb448: slot 0 of the type table of .?AV?$SettingT@VINISettingCollection@@@@: the one table of that type in each exe (0x4aff448, 0x4afb448), 3 slots in both; the other rules agree
- 0x4cb9958 -> 0x4cb57b8: slot 0 of the type table of .?AV?$TypeErasedFuncImpl@V<lambda_b0651812a17537ef4a7a2b5bd049f263>@@XAEAVNiAVObject@@@detail@unique_function_impl@stl@@: the one table of that type in each exe (0x4cb9958, 0x4cb57b8), 5 slots in both, holding 0x6200a0, the port of 0x620c90 (matched for certain); the other rules agree
- 0x4cc0a60 -> 0x4cbe028: referred to at 0x1c5ee0+0x7 (1110 functions alike, none clearly the one by its body; by its place: number 3 of the 215 functions between 0x1c5e50 and 0x1c9100 (each matched for certain), all alike and as many in both; its references fit it (4) and not its 19 twin(s) between them)
- 0x4cc41b0 -> 0x4cc01b0: slot 0 of the type table of .?AVProcessLists@@: the one table of that type in each exe (0x4cc41b0, 0x4cc01b0), 2 slots in both, holding 0x1a88fe0, the port of 0x1a8bad0 (matched for certain); the other rules agree
- 0x4ef6330 -> 0x4ef2290: slot 0 of the type table of .?AVBoneData@BSSkin@@: the one table of that type in each exe (0x4ef6330, 0x4ef2290), 55 slots in both, holding 0x2bf6540, the port of 0x2bfa8a0 (matched for certain); the other rules agree
- 0x4ef64f0 -> 0x4ef2450: slot 0 of the type table of .?AVInstance@BSSkin@@: the one table of that type in each exe (0x4ef64f0, 0x4ef2450), 55 slots in both, holding 0x2bf6890, the port of 0x2bfabf0 (matched for certain); the other rules agree
- 0x58d7ab0 -> 0x58cfab0: 3 places just before it referred to agree (0x58d7950, 0x58d7954, 0x58d7958); the same from there through it but for 25 pointer(s) into the image
- 0x5eff4d8 -> 0x5ef7718: referred to at 0x1a051f0+0x110 (the one function alike (one of its kind in both))
- 0x5f4e548 -> 0x5f467a8: referred to at 0x1c5ee0+0xe (1110 functions alike, none clearly the one by its body; by its place: number 3 of the 215 functions between 0x1c5e50 and 0x1c9100 (each matched for certain), all alike and as many in both; its references fit it (4) and not its 19 twin(s) between them)
- 0x5fb7bf0 -> 0x5fafe70: referred to at 0x19038a0+0x292 (the one function alike (one of its kind in both)); 11 more reference(s) to it agree
- 0x5fe1908 -> 0x5fd9b88: referred to at 0xb48040+0x25b (the one function alike (one of its kind in both)); 1 more reference(s) to it agree
- 0x61e51f0 -> 0x61dd460: referred to at 0xb48040+0x4ab (the one function alike (one of its kind in both)); 11 more reference(s) to it agree

## CreationEngine\PhysicsProbe.cpp: 42 of 42
- 0x50b8d0 -> 0x50b150: the one function alike (one of its kind in both), 0x0 in
- 0x5a10e0 -> 0x5a0520: the one function alike (one of its kind in both), 0x0 in
- 0xb3fd60 -> 0xb3f5d0: the one function alike (one of its kind in both), 0x0 in
- 0xb48040 -> 0xb47920: the one function alike (one of its kind in both), 0x0 in
- 0xc3bb30 -> 0xc3b320: the one function alike (one of its kind in both), 0x0 in
- 0x1027b70 -> 0x1027410: the one function alike (one of its kind in both), 0x0 in
- 0x19667d0 -> 0x1963f20: by hand (manual_1_16_244.txt line 123: the game's equip: its start)
- 0x19813d0 -> 0x197eb20: the one function alike (one of its kind in both), 0x0 in
- 0x1fc8df0 -> 0x1fc5780: the one function alike (one of its kind in both), 0x0 in
- 0x1fce980 -> 0x1fcb310: the one function alike (one of its kind in both), 0x0 in
- 0x1fd0538 -> 0x1fccf48: the one function alike (one of its kind in both), 0x808 in
- 0x1fd0598 -> 0x1fccfa8: the one function alike (one of its kind in both), 0x868 in
- 0x20660f0 -> 0x2062f50: the one function alike (one of its kind in both), 0x0 in
- 0x22371e0 -> 0x2234600: the one function alike (one of its kind in both), 0x0 in
- 0x235a310 -> 0x2357d20: the one function alike (one of its kind in both), 0x0 in
- 0x23602d0 -> 0x235dce0: the one function alike (one of its kind in both), 0x0 in
- 0x2360410 -> 0x235de20: the one function alike (one of its kind in both), 0x0 in
- 0x23605e0 -> 0x235dff0: the one function alike (one of its kind in both), 0x0 in
- 0x2360d80 -> 0x235e790: the one function alike (one of its kind in both), 0x0 in
- 0x2361130 -> 0x235eb40: the one function alike (one of its kind in both), 0x0 in
- 0x23640f0 -> 0x2361b00: the one function alike (one of its kind in both), 0x0 in
- 0x236aff0 -> 0x2368a00: the one function alike (one of its kind in both), 0x0 in
- 0x23781c0 -> 0x2375b90: by hand (manual_1_16_244.txt line 118: the collision object's world update (above))
- 0x2378b10 -> 0x23764e0: the one function alike (one of its kind in both), 0x0 in
- 0x2378d10 -> 0x23766e0: the one function alike (one of its kind in both), 0x0 in
- 0x2378df0 -> 0x23767c0: the one function alike (one of its kind in both), 0x0 in
- 0x2378fd0 -> 0x23769a0: the one function alike (one of its kind in both), 0x0 in
- 0x2379140 -> 0x2376b10: (no unwind entry: code 0x2379140-0x237915e between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x2379c20 -> 0x23775f0: the one function alike (one of its kind in both), 0x0 in
- 0x237a890 -> 0x2378260: the one function alike (one of its kind in both), 0x0 in
- 0x237cb20 -> 0x237a4f0: the one function alike (one of its kind in both), 0x0 in
- 0x239b500 -> 0x2398ef0: the one function alike (one of its kind in both), 0x0 in
- 0x23a54c0 -> 0x23a2eb0: the one function alike (one of its kind in both), 0x0 in
- 0x23c9e70 -> 0x23c7860: the one function alike (one of its kind in both), 0x0 in
- 0x23e3b80 -> 0x23e1570: the one function alike (one of its kind in both), 0x0 in
- 0x2bd9330 -> 0x2bd5000: the one function alike (one of its kind in both), 0x0 in
- 0x2be93f0 -> 0x2be50a0: the one function alike (one of its kind in both), 0x950 in
- 0x2bfc490 -> 0x2bf8130: (no unwind entry: code 0x2bfc490-0x2bfc4b1 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x2bfc590 -> 0x2bf8230: the nearest of 2 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x3062ac0 -> 0x305edc0: the one function alike (one of its kind in both), 0x0 in
- 0x3086e40 -> 0x3083140: the one function alike (one of its kind in both), 0x0 in
- 0x5fe4bc8 -> 0x5fdce60: referred to at 0x23c9e70+0x1ab6 (the one function alike (one of its kind in both))

## CreationEngine\ShadowShare.cpp: 2 of 2
- 0x29aa65a -> 0x29a63da: the one function alike (one of its kind in both), 0x13aa in
- 0x29aad36 -> 0x29a6ab6: the one function alike (one of its kind in both), 0x1a86 in

## CreationEngine\StereoView.cpp: 23 of 23
- 0x7ca610 -> 0x7c9be0: the one function alike (one of its kind in both), 0x0 in
- 0x840c4f -> 0x8403bf: the one function alike (one of its kind in both), 0x23f in
- 0x841a90 -> 0x841200: the one function alike (one of its kind in both), 0x0 in
- 0x841ba0 -> 0x841310: the one function alike (one of its kind in both), 0x0 in
- 0x841e50 -> 0x8415c0: the one function alike (one of its kind in both), 0x0 in
- 0xd784a0 -> 0xd77e70: the one function alike (one of its kind in both), 0x0 in
- 0x22c9910 -> 0x22c7320: the one function alike (one of its kind in both), 0x0 in
- 0x22c9d20 -> 0x22c7730: the one function alike (one of its kind in both), 0x0 in
- 0x22ca0c0 -> 0x22c7ad0: the one function alike (one of its kind in both), 0x0 in
- 0x248f480 -> 0x248d200: the one function alike (one of its kind in both), 0x0 in
- 0x2925647 -> 0x2921397: the one function alike (one of its kind in both), 0x1907 in
- 0x298cc36 -> 0x29889d6: the one function alike (one of its kind in both), 0x2b6 in
- 0x2a66270 -> 0x2a61fa0: the one function alike (one of its kind in both), 0x0 in
- 0x2b7be10 -> 0x2b77b40: (no unwind entry: code 0x2b7be10-0x2b7be21 between int3 paddings) the one function alike (one of its kind in both), 0x0 in
- 0x2be6f30 -> 0x2be2be0: the one function alike (one of its kind in both), 0x0 in
- 0x2be71c0 -> 0x2be2e70: the one function alike (one of its kind in both), 0x0 in
- 0x2be8aa0 -> 0x2be4750: the one function alike (one of its kind in both), 0x0 in
- 0x4b4ccb0 -> 0x4b48c98: slot 0 of the type table of .?AV?$_Ref_count_obj2@U?$atomic@M@std@@@std@@: the one table of that type in each exe (0x4b4ccb0, 0x4b48c98), 4 slots in both; the other rules agree
- 0x4ee7ad0 -> 0x4ee3a38: slot 10 of the type table of .?AVRenderViewSubGraph@CreationRendererPrivate@@: the one table of that type in each exe (0x4ee7a80, 0x4ee39e8), 11 slots in both, holding 0x2b77b40, the port of 0x2b7be10 (matched for certain); the other rules agree
- 0x597d178 -> 0x5975168: referred to at 0x7ca610+0x13 (the one function alike (one of its kind in both)); 24 more reference(s) to it agree
- 0x5e68158 -> 0x5e60398: referred to at 0x402c10+0x65 (the one function alike (one of its kind in both)); 119 more reference(s) to it agree
- 0x5e68180 -> 0x5e603c0: referred to at 0x402c10+0x75 (the one function alike (one of its kind in both)); 92 more reference(s) to it agree
- 0x5e821e0 -> 0x5e7a420: referred to at 0x5a10e0+0x1df5 (the one function alike (one of its kind in both)); 5 more reference(s) to it agree

## CreationEngine\TempleGestures.cpp: 14 of 14
- 0x12bd3b6 -> 0x12bcdd6: the one function alike (one of its kind in both), 0x266 in
- 0x12bd3bb -> 0x12bcddb: the one function alike (one of its kind in both), 0x26b in
- 0x12bd3c2 -> 0x12bcde2: the one function alike (one of its kind in both), 0x272 in
- 0x12bd3cb -> 0x12bcdeb: the one function alike (one of its kind in both), 0x27b in
- 0x12bd3d2 -> 0x12bcdf2: the one function alike (one of its kind in both), 0x282 in
- 0x18c88a0 -> 0x18c6090: the nearest of 2 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x1a68689 -> 0x1a65c59: the one function alike (one of its kind in both), 0x9 in
- 0x22f9940 -> 0x22f7380: the one function alike (one of its kind in both), 0x0 in
- 0x24397d0 -> 0x2437550: (no unwind entry: code 0x24397d0-0x24397de between int3 paddings) the nearest of 3 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x4d702e0 -> 0x4d6c308: slot 0 of the type table of .?AVBSLight@@: the one table of that type in each exe (0x4d702e0, 0x4d6c308), 85 slots in both, holding 0x24370f0, the port of 0x2439370 (matched for certain); the other rules agree
- 0x4d77a80 -> 0x4d73a70: slot 0 of the type table of .?AVBSShadowLight@@: the one table of that type in each exe (0x4d77a80, 0x4d73a70), 85 slots in both, holding 0x24a5440, the port of 0x24a76c0 (matched for certain); the other rules agree
- 0x5e794e8 -> 0x5e71728: referred to at 0xb790c0+0xac (the one function alike (one of its kind in both)); 9 more reference(s) to it agree
- 0x5edbe77 -> 0x5ed40c7: referred to at 0x1a68680+0x9 (the one function alike (one of its kind in both)); 3 more reference(s) to it agree
- 0x61e70c0 -> 0x61df328: referred to at 0x12bd150+0x141 (the one function alike (one of its kind in both)); 2 more reference(s) to it agree

## CreationEngine\VrSettingsTab.cpp: 58 of 58
- 0x38caa0 -> 0x38c4e0: 55 functions alike, none clearly the one by its body; by its place: number 8 of the 10 functions between 0x38c790 and 0x38cb50 (each matched for certain), all alike and as many in both; its references fit it (2) and not its 2 twin(s) between them, 0x0 in
- 0x38cad0 -> 0x38c510: 55 functions alike, none clearly the one by its body; by its place: number 9 of the 10 functions between 0x38c790 and 0x38cb50 (each matched for certain), all alike and as many in both; its references fit it (2) and not its 2 twin(s) between them, 0x0 in
- 0x14d68b0 -> 0x14d6350: the one function alike (one of its kind in both), 0x0 in
- 0x1511a60 -> 0x1511500: the one function alike (one of its kind in both), 0x0 in
- 0x1511a7e -> 0x151151e: the one function alike (one of its kind in both), 0x1e in
- 0x1511ba4 -> 0x1511644: the one function alike (one of its kind in both), 0x44 in
- 0x1512def -> 0x151288f: the one function alike (one of its kind in both), 0x128f in
- 0x1515d70 -> 0x1515810: the one function alike (one of its kind in both), 0x0 in
- 0x1515d8e -> 0x151582e: the one function alike (one of its kind in both), 0x1e in
- 0x1515ea9 -> 0x1515949: the one function alike (one of its kind in both), 0x39 in
- 0x1517036 -> 0x1516ad6: the one function alike (one of its kind in both), 0x11c6 in
- 0x1518550 -> 0x1517ff0: the one function alike (one of its kind in both), 0x0 in
- 0x151856e -> 0x151800e: the one function alike (one of its kind in both), 0x1e in
- 0x1518681 -> 0x1518121: the one function alike (one of its kind in both), 0x31 in
- 0x151a570 -> 0x151a010: the one function alike (one of its kind in both), 0x0 in
- 0x151a58e -> 0x151a02e: the one function alike (one of its kind in both), 0x1e in
- 0x151ae50 -> 0x151a8f0: the one function alike (one of its kind in both), 0x0 in
- 0x151ae7a -> 0x151a91a: the one function alike (one of its kind in both), 0x2a in
- 0x151ae91 -> 0x151a931: the one function alike (one of its kind in both), 0x41 in
- 0x151b7fe -> 0x151b29e: the one function alike (one of its kind in both), 0x9ae in
- 0x151b80b -> 0x151b2ab: the one function alike (one of its kind in both), 0x9bb in
- 0x151bed0 -> 0x151b970: the nearest of 13 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x151bfb0 -> 0x151ba50: the one function alike (one of its kind in both), 0x0 in
- 0x151bfca -> 0x151ba6a: the one function alike (one of its kind in both), 0x1a in
- 0x151bfcd -> 0x151ba6d: the one function alike (one of its kind in both), 0x1d in
- 0x151bfd0 -> 0x151ba70: the one function alike (one of its kind in both), 0x20 in
- 0x151bfdd -> 0x151ba7d: the one function alike (one of its kind in both), 0x2d in
- 0x151bfe5 -> 0x151ba85: the one function alike (one of its kind in both), 0x35 in
- 0x151bff7 -> 0x151ba97: the one function alike (one of its kind in both), 0x47 in
- 0x151c003 -> 0x151baa3: the one function alike (one of its kind in both), 0x53 in
- 0x1520d10 -> 0x15207b0: the one function alike (one of its kind in both), 0x0 in
- 0x1521160 -> 0x1520c00: the one function alike (one of its kind in both), 0x0 in
- 0x152bb90 -> 0x152b630: the one function alike (one of its kind in both), 0x0 in
- 0x152bba5 -> 0x152b645: the one function alike (one of its kind in both), 0xf in
- 0x1530f50 -> 0x1530850: the one function alike (one of its kind in both), 0x0 in
- 0x1530f6f -> 0x153086f: the one function alike (one of its kind in both), 0x1f in
- 0x1532c70 -> 0x1532570: the nearest of 2 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x1532c8f -> 0x153258f: the nearest of 2 functions alike (as many in both) to where its neighbours moved, 0x1f in
- 0x1535060 -> 0x1534960: the one function alike (one of its kind in both), 0x0 in
- 0x1535ff0 -> 0x15358f0: the nearest of 3 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x1536490 -> 0x1535d90: the one function alike (one of its kind in both), 0x0 in
- 0x1536930 -> 0x1536230: the nearest of 57 functions alike (as many in both) to where its neighbours moved, 0x0 in
- 0x1537d50 -> 0x1537650: the one function alike (one of its kind in both), 0x0 in
- 0x153b57b -> 0x153ae7b: the one function alike (one of its kind in both), 0x5b in
- 0x153b58f -> 0x153ae8f: the one function alike (one of its kind in both), 0x6f in
- 0x153b5a6 -> 0x153aea6: the one function alike (one of its kind in both), 0x86 in
- 0x153b5bd -> 0x153aebd: the one function alike (one of its kind in both), 0x9d in
- 0x254e600 -> 0x254c340: the one function alike (one of its kind in both), 0x0 in
- 0x254e690 -> 0x254c3d0: the one function alike (one of its kind in both), 0x0 in
- 0x28cef30 -> 0x28caea0: by hand (manual_1_16_244.txt line 55: push r12; sub rsp,40h; cmp qword [rcx],0; mov r12,rcx (1.16.236: r13))
- 0x28cfb80 -> 0x28cba80: by hand (manual_1_16_244.txt line 61: its start through lea rbp,[rsp-1Fh])
- 0x4b076d8 -> 0x4b03740: slot 0 of the type table of .?AV?$TUIValue@I@@: the one table of that type in each exe (0x4b076d8, 0x4b03740), 12 slots in both; the other rules agree
- 0x4b078a8 -> 0x4b03910: slot 0 of the type table of .?AV?$TUIValue@_N@@: the one table of that type in each exe (0x4b078a8, 0x4b03910), 12 slots in both; the other rules agree
- 0x4b1be50 -> 0x4b17ec0: slot 13 of the type table of .?AVStringUIValue@@: the one table of that type in each exe (0x4b1bde8, 0x4b17e58), 14 slots in both, holding 0x254c3d0, the port of 0x254e690 (matched for certain); the other rules agree
- 0x4c717a0 -> 0x4c6e280: slot 1 of the type table of .?AVSettingsDataModel@@ (its part at +0x78): the one table of that type in each exe (0x4c71798, 0x4c6e278), 2 slots in both, holding 0x1511500, the port of 0x1511a60 (matched for certain); the other rules agree
- 0x4c717b8 -> 0x4c6e238: slot 1 of the type table of .?AVSettingsDataModel@@ (its part at +0x80): the one table of that type in each exe (0x4c717b0, 0x4c6e230), 2 slots in both, holding 0x1515810, the port of 0x1515d70 (matched for certain); the other rules agree
- 0x4c71830 -> 0x4c6e250: slot 1 of the type table of .?AVSettingsDataModel@@ (its part at +0x88): the one table of that type in each exe (0x4c71828, 0x4c6e248), 2 slots in both, holding 0x1517ff0, the port of 0x1518550 (matched for certain); the other rules agree
- 0x4c71848 -> 0x4c6e120: slot 1 of the type table of .?AVSettingsDataModel@@ (its part at +0x90): the one table of that type in each exe (0x4c71840, 0x4c6e118), 2 slots in both, holding 0x151a010, the port of 0x151a570 (matched for certain); the other rules agree

## CreationEngine\WorkbenchCamera.cpp: 3 of 3
- 0x1e91d83 -> 0x1e8f2e3: the one function alike (one of its kind in both), 0xa3 in
- 0x1e91fe7 -> 0x1e8f547: the one function alike (one of its kind in both), 0x307 in
- 0x61e51f0 -> 0x61dd460: referred to at 0xb48040+0x4ab (the one function alike (one of its kind in both)); 11 more reference(s) to it agree

## CreationEngine\body\BodyIK.cpp: 4 of 4
- 0x1a41d50 -> 0x1a3f340: the one function alike (one of its kind in both), 0x0 in
- 0x4c44498 -> 0x4c40478: slot 0 of the type table of .?AVAimModule@EquippedWeapon@@: the one table of that type in each exe (0x4c44498, 0x4c40478), 21 slots in both, holding 0x1292160, the port of 0x12927a0 (matched for certain); the other rules agree
- 0x4d702e0 -> 0x4d6c308: slot 0 of the type table of .?AVBSLight@@: the one table of that type in each exe (0x4d702e0, 0x4d6c308), 85 slots in both, holding 0x24370f0, the port of 0x2439370 (matched for certain); the other rules agree
- 0x4d77a80 -> 0x4d73a70: slot 0 of the type table of .?AVBSShadowLight@@: the one table of that type in each exe (0x4d77a80, 0x4d73a70), 85 slots in both, holding 0x24a5440, the port of 0x24a76c0 (matched for certain); the other rules agree

## CreationEngine\models\GameFlow.cpp: 1 of 1
- 0x4cc2180 -> 0x4cbb918: slot 0 of the type table of .?AVPlayerTraversalState@@: the one table of that type in each exe (0x4cc2180, 0x4cbb918), 17 slots in both; the other rules agree

