# EsriGdbAPIWrapperCore

对 Esri FileGDB API 原生库（Linux：`libFileGDBAPI.so`；Windows：`FileGDBAPI.dll`）
的 .NET 封装，供第三方 C# / VB.NET / F# 程序直接调用。已实现并验证的接口覆盖官方
样例中的 **ExecutingSQL、Querying、Editing、Domains、TableSchemaXML、
CreateDeleteGDB、CompactFileGDB**（**不含** Display 样例的几何解析 / WKT 转换）：

- 目录浏览（数据集类型 / 子数据集 / 查询名 / 数据集定义）
- 打开表与字段元数据（类型、长度、可空、表定义 XML）
- `Table.Search`：属性条件 + 空间包络 + 子字段，逐行流式读取
- 行编辑：插入 / 更新 / 删除（含 NULL 与几何）
- 建库 / 删库 / 压缩、建表 / 建字段 / 删字段 / 建索引
- 域（Domain）列表 / 定义读取 / 创建 / 删除

```
C# 调用方
   │  P/Invoke (UTF-8, 纯 C ABI)
   ▼
libesrigdbnative.so / esrigdbnative.dll   ← native/ 本项目自己编译的桥接层
   │  C++ 调用
   ▼
libFileGDBAPI.so + libfgdbunixrtl.so      ← Esri 官方原生库（lib/，Linux）
FileGDBAPI.dll                            ← Esri 官方原生库（lib/，Windows）
   ▼
xxx.gdb 文件型地理数据库
```

之所以需要中间的 C 桥接层：FileGDB API 是 C++ 接口（`std::wstring` / `std::vector` /
C++ 对象），跨语言边界无法直接 P/Invoke。桥接层把它转换成纯 C 结构体 + UTF-8 字符串。

## 目录结构

```
EsriGdbAPIWrapperCore/
├── include/                      Esri FileGDB API 头文件（官方）
├── lib/                          Esri FileGDB API 运行库（官方）
│   ├── libFileGDBAPI.so          Linux 运行库
│   ├── libfgdbunixrtl.so
│   ├── FileGDBAPI.dll            Windows 运行库
│   ├── FileGDBAPI.lib            Windows 导入库（MSVC 链接必需）
│   └── esrigdbnative.dll         可选：预编译桥接层（无 cmake 时的回退）
├── native/                       ★ 本项目：C ABI 桥接层（按职责拆分，避免单文件过大）
│   ├── EsriGdbNative.h           对外 C 接口定义（也是 P/Invoke 的依据）
│   ├── EsriGdbBridge.h           内部共享声明：句柄结构体 + 辅助函数/注册表（非 ABI）
│   ├── EsriGdbNative.cpp         导出的 extern "C" 实现（唯一导出符号的文件）
│   ├── EsriGdbBridgeUtil.cpp     UTF-8/base64、错误报告、值转换、路径、C 编组
│   ├── EsriGdbBridgeXml.cpp      XML 归一化 + 表定义校验（防 Esri 解析器崩溃）
│   ├── EsriGdbBridgeRegistry.cpp 句柄表与对象生命周期（库 → 表 → 游标级联关闭）
│   ├── Makefile                  Linux：生成 native/build/libesrigdbnative.so
│   └── CMakeLists.txt            跨平台：Windows 用 MSVC 生成
│                                 native/build-win/bin/esrigdbnative.dll
├── build/
│   └── EsriGdbNative.targets     MSBuild 集成：按平台编译桥接层并把原生库拷到输出目录
├── src/EsriGdbWrapper/           ★ 本项目：.NET 类库（对外封装）
│   ├── Geodatabase.cs            Open/Create/Delete/Compact、ExecuteSql(Cursor)、目录浏览、
│   │                             域、OpenTable/CreateTable、Close/Dispose
│   ├── Table.cs                  表：字段元数据、Search、InsertRow/Update/Delete、
│   │                             AddField/DeleteField/AddIndex、Close
│   ├── QueryCursor.cs            流式游标（ReadNext / foreach）+ 原生结构编组
│   ├── QueryResult.cs            一次性查询结果（字段元数据 + 全部行）
│   ├── Row.cs                    行访问器（按下标 / 按列名 / 强类型取值）
│   ├── Field.cs, FieldType.cs    字段元数据（含 Length / IsNullable）
│   ├── Envelope.cs               空间过滤包络（xmin, ymin, xmax, ymax）
│   ├── ICursorOwner.cs           游标归属抽象（Geodatabase / Table 关闭时级联关游标）
│   ├── ValueConverter.cs         UTF-8 文本 → CLR 类型（读）；CLR 值 → 文本（写）
│   ├── NativeMethods.cs          P/Invoke 声明
│   └── EsriGdbException.cs       统一异常（带原生错误码）
├── samples/EsriGdbWrapper.Sample/  控制台示例（SQL + 目录 + 表结构 + 搜索 + 编辑 +
│                                   域 + 建/压/删库）
└── EsriGdbAPIWrapperCore.slnx
```

## 原生 C ABI（`native/EsriGdbNative.h`）

| 函数 | 说明 |
|---|---|
| `egdb_open` / `egdb_close` | 打开 / 关闭地理库，返回 `egdb_handle`；关闭时**级联**关闭它名下的表和游标 |
| `egdb_create_geodatabase` / `egdb_delete_geodatabase` | 新建 / 删除地理库（新建返回一个已打开的 handle） |
| `egdb_compact_database` | 压缩地理库（须先关闭它名下的表） |
| `egdb_execute_sql_cursor` | 执行 SQL，返回游标 `egdb_cursor*`（**流式**） |
| `egdb_cursor_field_count/name/type/length/nullable` | 游标列元数据（读第一行之前即可获取，含长度、可空） |
| `egdb_cursor_next` | 取下一行：`0` = 有行、`1` = 结束、其它 = 错误码 |
| `egdb_cursor_close` / `egdb_free_row` | 释放游标 / 行（`close` 可重复调用） |
| `egdb_execute_sql` / `egdb_free_table` | 一次性返回完整结果集（内部即游标循环，非 SELECT 返回空表） |
| `egdb_get_dataset_types` / `egdb_get_child_datasets` | 目录浏览：数据集类型、某父路径下的子数据集（空串 = 根 `\`） |
| `egdb_get_query_name` / `egdb_get_dataset_definition` | 相对查询名解析、数据集定义 XML |
| `egdb_get_domains` / `egdb_get_domain_definition` | 域列表 / 域定义 XML |
| `egdb_create_domain` / `egdb_delete_domain` | 创建 / 删除域（XML 进） |
| `egdb_open_table` / `egdb_close_table` | 打开 / 关闭表，返回 `egdb_tableobj*`；关闭表会**级联**关它名下的游标 |
| `egdb_table_field_count/name/type/length/nullable` | 字段元数据（含长度、可空，模式变更后自动刷新） |
| `egdb_table_get_definition` | 表定义 XML |
| `egdb_table_search` | 子字段 + where 条件 + 可选包络 → 流式游标 |
| `egdb_table_insert` | 按列名 / 值数组插入，返回 `outNewOid`（NULL = SQL NULL，几何 / blob 为 base64） |
| `egdb_table_update` / `egdb_table_delete` | 按 where + 可选包络更新 / 删除，返回 `outAffected`（条件为空 = 全表） |
| `egdb_create_table` | 由表定义 XML 建表 |
| `egdb_table_add_field` / `egdb_table_delete_field` / `egdb_table_add_index` | 模式变更（XML 进 / XML 出，字段缓存随之刷新） |

所有字符串均为 UTF-8；返回内存全部由桥接层分配，用 `egdb_free_string` /
`egdb_free_string_array` / `egdb_free_table` / `egdb_free_row` 释放。

**XML 归一化**：`CreateTable` / `AddField` / `AddIndex` / `CreateDomain` 接受的 XML
由桥接层先做归一化——去掉 BOM 与前导空白、给根元素补 `esri:` 前缀、补齐缺失的
`xmlns:xsi/xs/esri` 声明、并把根元素强制为对应的类型（`esri:Field` /
`esri:Index` / `esri:Domain`）。因此**手写的裸 XML 片段**（例如
`<Field><Name>X</Name>...</Field>`）也能直接使用；域定义取出后不改一字节即可
重新 `CreateDomain`。

**表定义校验**：Esri 的 `CreateTable` 对缺少 `<CLSID>` 的定义会直接**段错误**
（整个进程崩溃），对缺少 `<Indexes>` 或 `<Indexes>` 排在 `<CLSID>` 之后的定义则只报
"General function failure"。桥接层先校验这几点，缺什么、顺序错在哪都会以
`EGDB_E_INVALIDARG` + 明确文案返回，不会让调用方进程挂掉。

**路径归一化**：数据集路径缺少前导 `\` 时自动补上（`Cities` → `\Cities`），
`GetChildDatasets("")` 视为根目录 `\`。

## 构建

### Linux

依赖：g++、make、.NET SDK（示例按 .NET 10 编写，可自行改 `TargetFramework`）。

```bash
dotnet build EsriGdbAPIWrapperCore.slnx
```

构建时 `build/EsriGdbNative.targets` 会自动：

1. 执行 `make -C native` 编译 `libesrigdbnative.so`（`-Wl,-rpath,$ORIGIN` +
   `--disable-new-dtags`，即 DT_RPATH——只有 DT_RPATH 才会传递到间接依赖，
   否则 `libFileGDBAPI.so` 找不到同目录下的 `libfgdbunixrtl.so`）；
2. 把 `libesrigdbnative.so`、`libFileGDBAPI.so`、`libfgdbunixrtl.so` 复制到输出目录，
   使程序目录自包含，无需设置 `LD_LIBRARY_PATH`。

单独编译桥接层：`make -C native`（或用 CMake：`cmake -S native -B native/build-cmake
&& cmake --build native/build-cmake`，两者产物等价）。

### Windows

Windows 上必须用 **MSVC**：`FileGDBAPI.dll` 是 MSVC 编译的 C++ 二进制（导出 C++ 类与
`std::wstring`），MinGW / 交叉编译出来的桥接层名字改编规则与 C++ ABI 都对不上，链接
不上也不兼容。

准备 `lib/`：

| 文件 | 用途 |
|---|---|
| `FileGDBAPI.lib` | 导入库，CMake 链接用（**没有它 CMake 会明确报错**） |
| `FileGDBAPI.dll` | 运行库，targets 会拷到输出目录 |
| `esrigdbnative.dll` | 可选：预编译桥接层，cmake 不可用时的回退 |

需要安装 Visual Studio 2022（或 Build Tools，含 C++ 工作负载）和 CMake，然后照常：

```powershell
dotnet build EsriGdbAPIWrapperCore.slnx
```

`build/EsriGdbNative.targets` 在 Windows 上改为：

1. `cmake -S native -B native/build-win -DCMAKE_BUILD_TYPE=Release` +
   `cmake --build native/build-win --config Release`
   → `native/build-win/bin/esrigdbnative.dll`（MSVC / C++11 / `/MD`，输出目录被 CMake
   固定到 `bin/`，与 VS 的多配置无关）；
2. 把它和 `lib/*.dll`（`FileGDBAPI.dll` 等）复制到输出目录；
3. **回退**：若 cmake 或 Visual Studio 不可用（或缺 `FileGDBAPI.lib`），构建只给警告
   （MSB3073），改用 `lib/esrigdbnative.dll`；两者都没有才提示“找不到桥接层”。

也可以完全手动构建：

```powershell
cmake -S native -B native/build-win
cmake --build native/build-win --config Release
```

不需要自动编译时，在工程里加 `<EsriGdbSkipNativeBuild>true</EsriGdbSkipNativeBuild>`
（只做拷贝，配合 `lib/esrigdbnative.dll` 使用）。

跨平台细节：

- P/Invoke 的库名是 `esrigdbnative`，.NET 会解析到 Linux 的
  `libesrigdbnative.so` 和 Windows 的 `esrigdbnative.dll`；
- 桥接层内部统一 UTF-8，与 `std::wstring` 互转时按平台处理 UTF-32（Linux）/
  UTF-16 代理对（Windows）；
- Esri 头文件在 Windows 分支写的是单下划线 `_declspec(dllimport)`，
  `native/EsriGdbBridge.h` 会把它映射成 `__declspec`；
- 两端都用 `/MD`（默认）：`std::wstring` 由 `FileGDBAPI.dll` 分配、在桥接层里释放，
  两侧必须共用同一个 CRT 堆。

## 使用示例

### 1. 流式读取（推荐，游标）

结果集**逐行从原生层拉取**，内存占用与行数无关，适合大表：

```csharp
using EsriGdb;

using var gdb = Geodatabase.Open("./Data/MyData.gdb");

// 方式 A：ReadNext 手动拉取
using (QueryCursor cursor = gdb.ExecuteSqlCursor("SELECT * FROM HugeTable"))
{
    // 字段元数据在读第一行之前就可用
    foreach (Field f in cursor.Fields)
        Console.WriteLine($"  {f.Name} [{f.Type}]");

    while (cursor.ReadNext() is Row row)
    {
        Console.WriteLine($"{row.GetString("CITY_NAME")}\t{row.GetInt32("POP1990")}");
    }
}

// 方式 B：foreach（同样是流式，单次遍历）
using (QueryCursor cursor = gdb.ExecuteSqlCursor("SELECT CITY_NAME FROM Cities"))
{
    foreach (Row row in cursor)
        Console.WriteLine(row.GetString("CITY_NAME"));
}
```

游标语义：

- **单趟**：只能遍历一次，第二次 `foreach` 抛 `InvalidOperationException`；
  读完之后再 `ReadNext()` 返回 `null`。
- **尽早释放**：`using` 结束（提前 `break` 也会在 `Dispose` 时）关闭原生查询，
  不会把剩余行读完。
- **归属**：游标随 `Geodatabase` 或 `Table` 的关闭/释放而关闭；关闭后再读会抛
  `EsriGdbException`（而不是悄悄返回空）。
- 非 SELECT 语句（UPDATE/INSERT/DELETE/DDL）返回 0 列，`ReadNext()` 立即为 `null`。

### 2. 一次性取回全部结果

```csharp
using var gdb = Geodatabase.Open("./Data/MyData.gdb");

// 1. 简单查询
QueryResult result = gdb.ExecuteSql(
    "SELECT CITY_NAME, POP1990 FROM Cities WHERE TYPE = 'city' AND OBJECTID < 10");

Console.WriteLine($"行数={result.Count}");
foreach (Field f in result.Fields)
    Console.WriteLine($"  {f.Name} [{f.Type}]");

foreach (Row row in result)
{
    string? name = row.GetString("CITY_NAME");
    int?    pop  = row.GetInt32("POP1990");   // SQL NULL -> null
    Console.WriteLine($"{name}\t{pop}");
}

// 2. SELECT * （所有字段类型自动映射）
QueryResult all = gdb.ExecuteSql("SELECT * FROM Cities WHERE OBJECTID < 4");
Dictionary<string, object?> record = all[0].ToDictionary();

// 3. 任意受支持的语句（UPDATE/DELETE/INSERT/DDL 也返回空结果集）
gdb.ExecuteSql("UPDATE CitiesNoGeo SET POP1990 = 65678 WHERE STATE_CITY = '0659962'");
```

运行示例（可传入任意 .gdb 与 SQL；加 `edit` 参数会额外演示写入，**会修改该 .gdb**）：

```bash
# 只读演示：SQL + 目录 + 表结构 + 搜索 + 域 + 建/压/删库（临时目录）
dotnet run --project samples/EsriGdbWrapper.Sample -- /path/to/xxx.gdb

# 追加插入/更新/删除/加删字段演示（请对副本操作）
dotnet run --project samples/EsriGdbWrapper.Sample -- /path/to/xxx.gdb edit
```

### 3. 目录浏览与表元数据

```csharp
using var gdb = Geodatabase.Open("./Data/MyData.gdb");

IReadOnlyList<string> types = gdb.GetDatasetTypes();                  // 全部数据集类型
IReadOnlyList<string> fcs   = gdb.GetChildDatasets(datasetType: "Feature Class");
IReadOnlyList<string> all   = gdb.GetChildDatasets();                 // 根目录 "\"
string queryName = gdb.GetQueryName("Cities");                        // 相对名
string xml       = gdb.GetDatasetDefinition("\\Cities", "Feature Class");

using Table table = gdb.OpenTable("Cities");      // "Cities" 或 "\Cities" 均可

foreach (Field f in table.Fields)
    Console.WriteLine($"{f.Name,-14} [{f.Type}] len={f.Length} nullable={f.IsNullable}");

Field? city = table.FindField("CITY_NAME");       // String, 40, 可空
string definition = table.GetDefinition();        // 表定义 XML
```

### 4. 条件 / 空间搜索（流式）

```csharp
using QueryCursor cursor = table.Search(
    subfields:   "CITY_NAME, POP1990",            // 省略 / null / "" = 全部字段
    whereClause: "POP1990 > 100000",              // 省略 = 不过滤
    envelope:    new Envelope(-100, 20, -60, 50)); // xmin, ymin, xmax, ymax（要素类坐标系单位）

foreach (Row row in cursor)
    Console.WriteLine($"{row.GetString("CITY_NAME")} {row.GetInt32("POP1990")}");
```

三个参数彼此独立、可任意组合，游标语义与 `ExecuteSqlCursor` 完全一致
（单趟、`using` 释放、归属表关闭即失效）。

### 5. 行编辑（插入 / 更新 / 删除）

```csharp
// 值可以是字典，也可以是 (列名, 值) 元组；null 表示 SQL NULL。
int oid = table.InsertRow(
    ("AREANAME", "Cabazon"),
    ("CLASS",    "town"),
    ("POP2000",  2939),
    ("Shape",    shapeBytes));          // 几何 = ShapeBuffer 原始字节

int updated = table.Update(
    new Dictionary<string, object?> { ["CLASS"] = "city" },
    whereClause: $"OBJECTID = {oid}");   // 返回受影响行数

int deleted = table.Delete($"OBJECTID = {oid}");
```

要点：

- 几何 / Blob 直接传 `byte[]`，桥接层转成 ShapeBuffer 所需的 base64；
- 写入前会按**表的字段缓存**校验：列名不存在、类型不符（如给 Integer 传
  "abc"）、字符串超出字段长度都会抛出带具体原因的 `EsriGdbException`，不会写脏数据；
- `Update` / `Delete` 的条件与包络都可省略——省略即全表；只传 `envelope`
  则按空间范围圈定（匹配 0 行时返回 0，不报错）；
- 出参：`InsertRow` 返回新 `OBJECTID`，`Update` / `Delete` 返回受影响行数。

### 6. 建库 / 建表 / 字段 / 索引 / 压缩 / 删库

```csharp
using Geodatabase created = Geodatabase.Create("./out/New.gdb");   // 返回已打开的库

using Table table = created.CreateTable(File.ReadAllText("Table.xml"));

table.AddField("<Field><Name>NOTES</Name><Type>esriFieldTypeString</Type>" +
               "<IsNullable>true</IsNullable><Length>50</Length><Precision>0</Precision>" +
               "<Scale>0</Scale><AliasName>Notes</AliasName><ModelName>NOTES</ModelName></Field>");
table.DeleteField("NOTES");
table.AddIndex(File.ReadAllText("Index.xml"));

created.Compact();          // 须先关闭该库名下的表
table.Close();
created.Close();
Geodatabase.Delete("./out/New.gdb");
```

表定义 XML 必须是完整的 `<esri:DataElement>`（`CatalogPath`、`Name`、`DatasetType`、
`HasOID`、`Fields`、`Indexes`、`CLSID`，且 `Indexes` 要在 `CLSID` 之前），
样例见 `samples/XMLsamples/Table.xml`；缺项会被桥接层拦下并说明缺什么
（否则 Esri API 会直接崩溃）。`AddField` / `AddIndex` 用**裸片段**即可，
桥接层自动补前缀与命名空间。

### 7. 域（Domain）

```csharp
IReadOnlyList<string> domains = gdb.GetDomains();
string xml = gdb.GetDomainDefinition(domains[0]);   // 取出的定义可原样重建

gdb.DeleteDomain(domains[0]);
gdb.CreateDomain(xml);
```

`GetDomainDefinition` 返回的 XML 不需要任何手工修改即可 `CreateDomain`
（桥接层会把根元素规整成 `esri:Domain`）。

## 类型映射

| FileGDB `FieldType` | C# 类型        | 说明                                   |
|---------------------|----------------|----------------------------------------|
| SmallInteger         | `short`        |                                        |
| Integer / OID        | `int`          | OID 走 `Row.GetOID`                    |
| Single               | `float`        |                                        |
| Double               | `double`       |                                        |
| String / XML         | `string`       | UTF-8                                  |
| Date                 | `DateTime`     | `yyyy-MM-ddTHH:mm:ss`（本地时间，无时区） |
| GUID / GlobalID      | `Guid`         |                                        |
| Blob / Geometry      | `byte[]`       | Geometry 为 FileGDB ShapeBuffer 原始字节（可自行反序列化为 WKT/GeoJSON） |
| Raster               | `null`         | 桥接层暂不解析栅格                     |

SQL NULL 一律为 `null`。任何解析失败都会保留原始字符串，不丢数据。

**写入方向**（`InsertRow` / `Update`）按同样的规则反向转换：`null` → SQL NULL，
`byte[]` → Geometry / Blob（转成 ShapeBuffer 需要的 base64），数值 / 布尔 / `DateTime`
→ 与读取格式一致的文本（`DateTime` 用 `yyyy-MM-ddTHH:mm:ss`），其余类型走
`IFormattable`（`InvariantCulture`）。类型不匹配时**不写入**，直接抛
`EsriGdbException` 并指出字段与取值。

## 错误处理

所有原生失败都抛出 `EsriGdbException`，`ErrorCode` 是 FileGDB API 的原始 `fgdbError`
（例如 `-2147220655` = 表不存在），`Message` 为 UTF-8 错误描述：

```
Error (-2147220655): Executing SQL 'SELECT * FROM NoSuchTable' failed:
  the SQL statement failed: The table was not found. [fgdbError=-2147220655 (0x80040351)]
```

## 第三方项目接入

1. 引用类库：

   ```xml
   <ProjectReference Include="path/to/EsriGdbAPIWrapperCore/src/EsriGdbWrapper/EsriGdbWrapper.csproj" />
   ```

2. 导入目标文件，保证三个 `.so` 落到你自己的输出目录（类库构建时只拷贝到它自己的
   bin，且首次全新构建时无法传递）：

   ```xml
   <Import Project="path/to/EsriGdbAPIWrapperCore/build/EsriGdbNative.targets" />
   ```

   已经构建过一次 `native/` 后，`.so` 也会通过常规内容项自动传递到引用方输出目录。

3. 运行，出错时按上面的异常处理方式捕获 `EsriGdbException`。

## 扩展更多接口（后续工作）

桥接层模式已经固定，新增一个函数只需三步：

1. `native/EsriGdbNative.h` + `EsriGdbNative.cpp`：增加 `extern "C"` 函数
   （UTF-8 进、纯 C 结构出，错误码 + 错误缓冲区返回）；需要新的内部工具函数时，
   声明写进 `native/EsriGdbBridge.h`，实现放在对应的 `EsriGdbBridge*.cpp`
   （util = 编码/值转换，xml = XML 归一化与校验，registry = 句柄与生命周期），
   新增的 `.cpp` 记得同时登记到 `native/Makefile` 的 `SRCS` 和
   `native/CMakeLists.txt` 的 `ESRI_GDB_BRIDGE_SOURCES`；
2. `src/EsriGdbWrapper/NativeMethods.cs`：增加对应 `DllImport`；
3. `src/EsriGdbWrapper/Geodatabase.cs` / `Table.cs` 等：包装成面向对象 API。

尚未覆盖（官方样例中明确排除或尚未实现）：

- **Display 样例**的几何解析：把 ShapeBuffer 转成 WKT / GeoJSON，以及坐标变换
  （当前几何只以 `byte[]` 原样进出，见类型映射）；
- 行对象句柄（`RowBuffer` / `SetXxx` 风格）：当前编辑统一走列名 + 值数组；
- 关系类、子类型、要素数据集（`CreateFeatureClass` / `CreateRelationshipClass`）、
  栅格目录；

## 备注

- 桥接层内维护了句柄表 + 互斥锁，多次 `Open` 互不影响；`Geodatabase` 和 `Table`
  都实现了 `IDisposable`（`using` 即可）并带终结器兜底。
- **级联释放**：`Table.Close()` 先关掉它名下的游标，`Geodatabase.Close()` 先关名下
  所有表（进而关掉这些表的游标）再关自己；之后对游标的任何读取都会抛
  `EsriGdbException`，不会悄悄返回空结果，也不会遗留原生句柄。
- 桥接层把同一个库 / 表句柄上的调用用互斥锁串行化，多线程并发读取同一个表是安全的；
  但单个 `QueryCursor` 是单趟的，不要跨线程共享同一个游标。
- 大结果集请用 `ExecuteSqlCursor()` / `Table.Search()` 流式读取（内存与行数无关）；
  `ExecuteSql()` 一次性返回全部行，只适合小结果集。
- 编辑接口直接改写磁盘上的 `.gdb`，请对副本操作。
- 平台：**Linux x64**（GCC + `libFileGDBAPI.so`）和 **Windows x64**
  （MSVC + `FileGDBAPI.dll`）。桥接层必须与 Esri 原生库同一工具链家族，
  否则 C++ ABI 对不上；见「构建 → Windows」。
