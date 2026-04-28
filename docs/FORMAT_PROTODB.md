# CPCW ProtoDB.bin Format

Prototype Database for Codename: Panzers Cold War — stores all game entity definitions (units, weapons, skills, squads, buildings, effects, etc.) in a hierarchical chunk-based binary format with a schema system.

## File Layout

```
OBJS Header (12 bytes)
  Object Data Tree
  Schema Section
```

### OBJS Header

| Offset | Size | Type | Description |
|--------|------|------|-------------|
| 0 | 4 | char[4] | Magic: `OBJS` |
| 4 | 4 | u32 LE | Total content size (= file_size - 8) |
| 8 | 4 | u32 LE | Schema section offset (from start of file) |

## Chunk Types

All chunks in the data tree use this pattern:

### OBJT — Object Container

```
Tag:      4 bytes "OBJT"
Size:     u32 LE (content size, starts after this field)
TypeID:   u16 LE (schema type ID, first 2 bytes of content)
Content:  VOBJ chunk + optional trailing VOBJ chunks
```

Total bytes: `8 + Size`

### VOBJ — Value Object

```
Tag:      4 bytes "VOBJ"
Size:     u32 LE (content size)
TypeID:   u16 LE (schema type ID, matches parent OBJT)
Version:  u16 LE (schema version, typically 1)
Fields:   [field values in schema-defined order]
```

Total bytes: `8 + Size`

### ARRY — Array

```
Tag:      4 bytes "ARRY"
Size:     u32 LE (content size, includes count field)
Count:    u32 LE (number of child OBJT elements)
Children: [OBJT chunks, Count times]
```

Total bytes: `8 + Size`

## VOBJ Field Data Encoding

Fields are stored sequentially in schema-defined order within the VOBJ content (after TypeID + Version).

| Type ID | Name | Storage | Size |
|---------|------|---------|------|
| 0x0001 | int32 | Signed 32-bit LE integer | 4 |
| 0x0002 | float | IEEE 754 single-precision LE | 4 |
| 0x0003 | bool | Single byte (0=false, 1=true) | 1 |
| 0x0004 | string | u16 LE length prefix + ASCII bytes | variable |
| 0x0011 | GUID | u16 LE length (36) + UUID string `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` | 38 |
| 0x0012 | reference | Same as GUID — references another object by UUID | 38 |
| 0x0017 | uint8 | Single unsigned byte | 1 |
| 0x0019 | int16 | Signed 16-bit LE integer | 2 |
| 0x0088 | inline object | Embedded OBJT or VOBJ chunk | variable |
| 0x0089 | inline object | Embedded OBJT or VOBJ chunk | variable |
| 0x039c | flags/string | u16 LE length prefix + ASCII bytes | variable |
| 0x0165 | blob | Raw bytes (size from schema field_size) | fixed |
| 0x898a | array | Embedded ARRY chunk | variable |

## Schema Section

Located at the offset specified in the OBJS header.

### SCHD — Schema Directory

```
Tag:          4 bytes "SCHD"
Size:         u32 LE (content size)
SchemaCount:  u16 LE
Unknown:      u16 LE
Schemas:      [SCHM entries, SchemaCount times]
```

### SCHM — Schema Definition

```
Tag:         4 bytes "SCHM"
Size:        u32 LE (content size)
NameLen:     u16 LE
Name:        ASCII string (e.g. "SPUnit", "SPGunner")
TypeID:      u16 LE (matches OBJT/VOBJ TypeID)
Version:     u16 LE
FieldCount:  u16 LE
Fields:      [field definitions, FieldCount times]
```

### Field Definition

```
NameLen:    u16 LE
Name:       ASCII string (e.g. "MaxHP", "FrontArmor")
FieldType:  u32 LE (see Field Data Encoding table)
FieldSize:  u32 LE (byte size for fixed types, 65535 for variable, 0 for embedded chunks)
```

## Object Tree Structure

The root object is always `SFolderObject` (TypeID 0x00AC) containing:
- `Name`: string (empty for root)
- `GUID`: UUID string
- `SubFolders`: array of nested `SFolderObject`
- `Objects`: array of game entity objects
- `EnableInEditor`: bool

### Top-Level Folders (21 categories)

Production Lists, Skills - Equipment, Units, Buildings, Drivers, Gunners, Objects, SkillSets, Camera, Deformer, Editor helpers, Effect Sets, Effects, External Help, Flag, FootSteps, Objective, Projectiles, Ruts, Sounds, Unit Decals

## Key Schemas

| TypeID | Name | Fields | Description |
|--------|------|--------|-------------|
| 0x00AC | SFolderObject | 5 | Tree organizer (Name, GUID, SubFolders[], Objects[], EnableInEditor) |
| 0x012E | SPUnit | 74 | Unit definition (HP, armor, speed, weapons, icons, nationality, etc.) |
| 0x0125 | SPGunner | 43 | Weapon stats (damage, range, reload, burst, scatter, etc.) |
| 0x0118 | SPDriver | 9 | Movement driver (speed, acceleration, type) |
| 0x012A | SPSquad | 6 | Squad composition (member refs, leader/member type) |
| 0x0150 | SProductionList | 5 | Unit production lists |
| 0x01AE | SPSkill | 13 | Base skill class (GUID, name, tooltip, icon, bonuses) |
| 0x0129 | SPProjectile | 9 | Projectile definition (velocity, gravity, type) |
| 0x0117 | SPDoodad | 18 | Destroyable object properties |
| 0x0115 | SPBuildingUnit | 7 | Building-specific properties |

## Statistics (main2.pak ProtoDB.bin)

- File size: 1,816,605 bytes
- Schemas: 117
- Total objects in tree: ~6,900
- Schema section: 15,185 bytes
