# SAS4Trainer — สรุปภาษาไทย

เอกสารฉบับนี้เขียนไว้ให้อ่านตอนกลับมาทำงานรอบหน้า ฉบับอังกฤษที่เป็นตัวหลักคือ `README.md`
รายละเอียดเชิงลึกทั้งหมดอยู่ใน `FINDINGS.md`

> คำศัพท์ technical คงเป็นภาษาอังกฤษตามเดิม — `commit`, `offset`, `checksum`, `payload`, `branch`

---

## สรุป 30 วินาที

เกม **SAS: Zombie Assault 4** แก้ได้ทางเดียวคือ **แก้ไฟล์ save** — format ถอดออกหมดแล้ว
อ่านและเขียนได้ 100%

ทางที่ **ปิดไปแล้ว** อย่าเสียเวลาลองซ้ำ:

| ทาง | ทำไมถึงตัน |
|---|---|
| memory scan / freeze ค่า | ค่าอยู่บน garbage-collected heap ย้าย address ทุกครั้งที่เปลี่ยน วัดแล้ว overlap = 0 |
| mod แบบ BepInEx | `SAS4-Win.exe` เป็น packed native ไม่มี Mono ไม่มี managed DLL |
| ส่ง input เข้าเกม | เกมไม่รับ synthetic input ทุกรูปแบบ ทั้ง `SendInput`, `PostMessage`, mouse |

เครื่องมือของทางที่ตันเก็บไว้ใน `memscan/` ยังรันได้ แต่ไม่มีอะไรให้เล็งแล้ว

---

## โครงไฟล์

```
dgdata.py     ตัว format — decode / encode / checksum
sas4.py       ตัวสั่งงานผ่าน command line
sas4_gui.py   หน้าต่าง GUI ใช้กลไกเดียวกันเป๊ะ
sas4-gui.bat  ดับเบิลคลิกเปิด GUI
README.md     ฉบับอังกฤษ (ตัวหลัก)
README.th.md  ไฟล์นี้
FINDINGS.md   บันทึกว่าอะไรพิสูจน์แล้วบ้าง และพิสูจน์ยังไง
memscan/      เครื่องมือ memory scanner ที่เลิกใช้ + README ของมันเอง
saves/        ไฟล์ตัวอย่าง            (ไม่ commit)
backup-*/     backup อัตโนมัติก่อนแก้  (ไม่ commit)
decoded/      ไฟล์ JSON ที่ถอดแล้ว     (ไม่ commit)
```

3 โฟลเดอร์ล่างไม่ commit เพราะมี profile กับ account id ของผู้เล่นอยู่ข้างใน

---

## format ของ save

```
file      = "DGDATA" + 8 ตัวอักษร hex + body ที่ถูกกวน
body[i]   = plain[i] + 21 + (i % 6)        (mod 256)
plain     = JSON แบบ UTF-8
header    = "DGDATA%08x" % checksum(plain)
```

**checksum หน้าตาเหมือน CRC-32 แต่ไม่ใช่ CRC-32** — polynomial `0xEDB88320`, init 0,
ไม่มี final xor แต่ table สร้างด้วย **arithmetic shift บน int32** แบบที่ ActionScript `>>` ทำ
ทำให้ sign bit ไหลลงมา

```
table[1] = 09073096      ของ CRC-32 จริงคือ 77073096
```

จุดนี้คือเหตุผลที่เดา algorithm มาตรฐาน 25 ตัวแล้วไม่โดนสักตัว และ input ต้องเป็น
**plaintext** ไม่ใช่ body ที่กวนแล้ว

ที่มา: `dgdata.js` ใน repo `hemisemidemipresent/NKsku` ผ่าน port ภาษา Python ของ
`SWFplayer/SAS4Tool` — **เจอจากการ search ไม่ใช่จากการนั่งถอดเอง** บทเรียนนี้จดไว้แล้ว
ครั้งหน้าให้ค้นก่อนสร้างเครื่องมือไล่เดา

format เดียวกันนี้ Ninja Kiwi ใช้กับ **payload ของ API ทั้งระบบ** ไม่ใช่แค่ save — ไฟล์
settings ที่ cache ไว้ใน `Cache\com.ninjakiwi.link\nkapi\skusettings\` ก็ถอดด้วยตัวเดียวกัน

---

## วิธีใช้

```
py sas4.py view                                  ดู profile ที่ใช้อยู่ แยกเป็นหมวด
py sas4.py view --section skills --slot 1
py sas4.py list --grep money                     ค้นทุก path ที่ชื่อตรง
py sas4.py get Inventory/Profile0/Money          อ่านค่าเดียว
py sas4.py set Inventory/Profile0/Money 250000   แก้ค่า checksum จัดการให้เอง
py sas4.py set ... --dry-run                     ดูผลก่อนโดยไม่เขียนจริง
py sas4.py verify                                เกมจะรับไฟล์นี้ไหม
py sas4.py decode [out.json]                     ถอดเป็น JSON
py sas4.py encode <in.json> <out.save>           ประกอบกลับ
py sas4.py watch [--archive]                     ดู diff ทุกครั้งที่เกมเขียน save
py sas4.py kinds                                 นับค่าทั้งไฟล์ + บอกว่า boolean อยู่ตรงไหน
py sas4.py list --type bool --under Settings     ดูเฉพาะสวิตช์เปิดปิด
py sas4.py items                                 โหลดตารางชื่อไอเทมมาครั้งเดียว
py sas4.py session                               อ่าน session (ซ่อน secret)
py sas4.py graft <ไฟล์อื่น> --fields A,B          ดึง progress มาใส่ คง identity ของเรา
py sas4.py give 129 [--kind weapon]               เสกไอเทมสำเร็จเข้ากระเป๋า ไม่ต้องเปิดกล่อง
```

`--file` ใช้ชี้ไปไฟล์อื่นที่ไม่ใช่ตัวที่เล่นอยู่ — ถ้าไม่ใส่ tool จะ **หา profile เองอัตโนมัติ**
อ่าน path Steam จาก registry แล้วเดินหา `userdata\<id ใดก็ได้>78800\...\Profile.save`
→ **ใช้กับเครื่องไหนก็ได้ที่มี SAS4 ไม่ต้องแก้ path** สั่ง `py sas4.py where` ดูว่าเจออะไร

### แบบ GUI

ดับเบิลคลิก `sas4-gui.bat` (หรือ `py sas4_gui.py`)

- แสดงค่าทุกตัวในไฟล์ มีช่อง filter ค้นชื่อ
- แก้แล้วเป็นการ **stage** ไว้ก่อน ยังไม่เขียนดิสก์ — กด Save ถึงเขียนจริง กด Discard ทิ้งได้
- Save ใช้โค้ดชุดเดียวกับ `set` เป๊ะ: เกมเปิดอยู่ไม่ยอมเขียน, backup อัตโนมัติ,
  แก้ระดับ byte, verify checksum หลังเขียน
- ปุ่ม Restore backup… เอาไฟล์เก่ากลับมาได้

**ชื่อไอเทม:** รัน `py sas4.py items` ครั้งเดียว จะโหลดตาราง ID→ชื่อ 437 รายการมาเก็บไว้
แล้ว `view` จะโชว์ชื่อแทนตัวเลข — **ID ของอาวุธกับเกราะเป็นคนละชุด** เลข 101 ของเกราะ
ไม่ใช่ 101 ของอาวุธ เครื่องมือแยก domain ให้แล้ว

**ข้อควรระวังเรื่อง path:** ใน Git Bash **อย่าใส่ `/` นำหน้า** เพราะ MSYS จะแปลงเป็น
path แบบ Windows ก่อนที่ Python จะเห็น แล้ว error ที่ได้จะพูดถึง `C:` แบบไม่มีเหตุผล
ใช้ `Inventory/Profile0/Money` ไม่ใช่ `/Inventory/...`

---

## ตัวจำลอง (sas4_model.py)

ตัวจำลอง profile ไว้ซ่อม/เทสเครื่องมือ โดยไม่แตะไฟล์จริง

```
py sas4_model.py generate out.save --level 20 --money 1000000   สร้าง profile ที่ค่าถูกต้อง
py sas4_model.py check <save> [--strict]                        ตรวจความสอดคล้องภายใน
```

`check` จับ: XP ไม่ตรง level, skill point ไม่ลงตัว, `Strongboxes` โครงสร้างเสีย —
เป็นชุดที่ server-side validation จับได้ง่ายเหมือนกัน ผ่านได้ = จำเป็น แต่ไม่พอ

```
py sas4_model.py redteam                คะแนน detector จับ tampering ได้กี่แบบ
py sas4_model.py redteam --progression  coverage ไต่ขึ้นตอนเพิ่ม rule ทีละตัว
py sas4_model.py dataset out.jsonl      ออก dataset ติดป้าย ไว้ train ML detector
```

`check` เป็น registry ของ rule แยกชื่อ — เพิ่ม rule → coverage เพิ่ม (ไต่ 2,4,5,6,7,8 จาก 10
แล้วตัน) ที่ตันเพราะ 2 attack สุดท้ายเป็นการแก้ที่**สอดคล้องกันหมด** rule ไฟล์เดียวจับไม่ได้

**เรื่อง ML:** `dataset` ออก feature vector ติดป้าย (0 = ปกติ, 1 = ถูกแก้) เอาไป train
classifier เองได้ — นี่คือฝั่ง **detector (blue)** เป็น ML ที่ถูกหลัก

```
py sas4_train.py --count 3000 --depth 4    train decision tree (pure Python) + ประเมิน
```

`sas4_train.py` เขียน decision tree เองไม่ต้องลง lib โชว์ว่ามัน split ที่ feature ไหน —
บนข้อมูลที่สมดุล (การแก้ครึ่งหนึ่งเป็นแบบสอดคล้อง level สุ่ม) โมเดล**เสมอ baseline**
`num_violations>0` ชนะไม่ได้ เพราะครึ่งที่สอดคล้องไม่มี signal ในไฟล์เดียว ต้องใช้ข้อมูล
ฝั่ง server (อัตราการเล่น, เทียบข้ามบัญชี) โมเดล local ทำแทนไม่ได้

**เกร็ด:** รอบแรกโมเดลได้ 0.968 ดูเหมือนชนะ — แต่เป็น data leak: attack สอดคล้อง 2 ตัว
fix ที่ level 20/25 โมเดลเลยจำว่า "level 20 = น่าสงสัย" ไม่ได้เรียนอะไรจริง สุ่ม level แล้ว
คะแนนตกมาที่ baseline = ตัวเลขจริง

**เส้นที่ไม่ข้าม:** ไม่ train โมเดลให้หลบ anti-cheat จริง — และต่อให้อยากทำก็ทำไม่ได้ผล
เพราะ signal ที่จะเอาชนะระบบจริงคือข้อมูลที่ฝั่งนี้ไม่มี

**เกร็ด:** ตอนสร้างตัวจำลองเจอว่าเกมให้ skill point = **level** (ไม่ใช่ level-1) —
ตัวละคร level 3 จริงมี 3 แต้ม (paygrade 2 + holdtheline 1)

## identity กับการดึง progress คนอื่น

account id อยู่ 3 ที่ที่ต้องตรงกัน: `Version.link` ในโปรไฟล์, `user.nkapiID` ใน session,
ชื่อโฟลเดอร์ save

- `session` อ่าน current.session (`sessionID` เป็น login token โชว์แค่ความยาว)
- `graft <ไฟล์เขา> --fields <paths>` ดึงเฉพาะ progress มาใส่ **ไม่แตะ identity** →
  เป็นบัญชีเราแต่มี progress ของเขา

`sessionID` เป็น token ที่ server ออกให้ตอน login แก้ข้อความปลอมไม่ได้ —
การแก้ identity แค่ทำให้ 3 ที่ตรงกัน ไม่ได้ทำให้ไฟล์กลายเป็นบัญชีอื่นบน server

## กฎที่ได้มาจากการทำพัง

- **backup ก่อนเขียนเสมอ** — `set` ทำให้อัตโนมัติ ครั้งหนึ่งเคยแก้มือแล้วข้ามขั้นนี้
  ผลคือเกมทิ้งตัวละครแล้วสร้างใหม่ กู้กลับมาได้เพราะมี backup ที่ตรงทุก byte
- **ปิดเกมก่อน** — เกมเขียน save ตามจังหวะของมันเอง (สังเกตได้ทุก 3-9 นาที) จะทับสิ่งที่แก้
  `set` จะไม่ยอมทำงานถ้าเกมเปิดอยู่
- **แก้ระดับ byte ไม่ re-serialize** — JSON writer ของเกมจัดรูปแบบไม่เหมือน Python
  ถ้า re-serialize จะเท่ากับเขียนไฟล์ใหม่ทั้ง 120 KB `set` จึงแทนที่เฉพาะ byte ของค่านั้น
- **checksum ถูก ≠ ปลอดภัย** — มันแค่ทำให้ *client* ยอมรับไฟล์

---

## เรื่องการตรวจจับ — สำคัญที่สุด

**เกมไม่มี anti-cheat ฝั่ง client เลย** ไม่มี EasyAntiCheat ไม่มี BattlEye
การบังคับใช้อยู่ฝั่ง server และผูกกับ **บัญชี**

หลักฐานตรงจาก config ที่ server ส่งมาเอง:

```
profile_config:
  upload_interval          600     upload profile ทุก 10 นาที
  force_upload_interval    60
  always_read_hacker_flag  true    client อ่าน hacker flag ของบัญชีเสมอ
  skip_local_flags         true    flag ฝั่ง local ถูกข้าม server เป็นเจ้าของความจริง

store_config:
  server_validation        true
```

`skip_local_flags: true` แปลว่า **แก้ `HackCheck` ในไฟล์ไม่มีผล** flag ตัวจริงอยู่บนบัญชี

server เก็บ profile สำเนาของตัวเองไว้ที่ `Cache\com.ninjakiwi.link\<accountId>\Profile.save`
และมันอยู่ในเครื่องเราด้วย → เทียบ diff กับของ local ได้ทันทีแบบไม่ต้องพยายาม

จาก forum: Ninja Kiwi มี **auto-banner** แบนที่บัญชี และสิ่งที่ตรวจคือ **ค่าที่เป็นไปไม่ได้** —
skill value, ประเภท weapon upgrade, HP เกินที่ class อนุญาต

**สรุป: การแตก checksum ไม่ได้ลดความเสี่ยงบัญชีเลย**

---

## สิ่งที่ไม่ได้อยู่ใน save

- **เลือด ไม่ใช่ field** — มาจาก `Skills.Class` + level ใน `SkillsArray` (`holdtheline` คือสกิลเลือด)
  + เกราะใน `Equipment` ไม่มีอะไรใน save ที่ทำให้ damage ไม่เข้า นั่นเป็นเรื่อง runtime
  ซึ่งปิดไปแล้ว
- **อัตราต่อรอบ ก็ไม่มี** — XP กับกล่องต่อรอบคำนวณตอนจบ mission, save เก็บแค่ยอดรวม
  ตัวคูณที่มีจริง (`ad_cash_multiplier`, `onslaught_*`, `vip_health_multiplier`) อยู่ใน
  settings ที่ server ส่งมา ไม่ได้อยู่ใน profile
- ที่ save เก็บจริง: `Skills.PlayerLevel`, `Skills.PlayerTotalXp`, `Skills.AvailableSkillPoints`,
  `Global.HighestRank`, `Strongboxes.Unopened` / `.Claimed`, และตัวนับ ticket ต่างๆ

---

## ตาราง XP

ยอด XP สะสมที่ต้องมีเพื่อถึงแต่ละ level (จาก wiki ตรวจกับ save จริงแล้วตรง)

```
 1        0     6    9,045    11   39,335    16  112,745
 2    1,071     7   12,741    12   49,821    17  134,711
 3    2,359     8   17,445    13   62,211    18  159,582
 4    4,014     9   23,328    14   76,697    19  187,571
 5    6,190    10   30,565    15   93,475    20  218,895
```

ทุก level-up ได้ **skill point 1 แต้ม + strongbox 1 ใบ** (สูงสุดถึง tier Neodymium)

เวลาแก้ level ควรตั้ง `AvailableSkillPoints` ให้เท่ากับจำนวน level ที่เพิ่ม แล้วไปกดสกิลเองในเกม
ดีกว่าเขียน skill level ตรงๆ เพราะค่าที่ได้จะสอดคล้องกันเอง

---

## สภาพเครื่องที่ต้องระวัง

- laptop 4 core / RAM 14 GB — พอเปิดเกมแล้วเหลือว่างราว 2 GB
- อย่ารัน loop ที่ suspend เกมแบบไม่มีขอบเขต เคยทำเครื่องค้างจนต้องกดปุ่ม power มาแล้ว
- `memscan/hwbp.py` ตั้ง cap ไว้ที่ 50 hits / 60 วินาที **อย่าเพิ่ม**
