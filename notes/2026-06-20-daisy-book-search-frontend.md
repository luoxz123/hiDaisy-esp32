# 2026-06-20 DAISY book search frontend

## 需求

创建一个简单、音频可访问的 React 界面，用于测试 HiLily DAISY 图书搜索、服务端反馈音频和图书音频播放。

## 实现

- 在 `frontend/` 新建独立 Vite + React 应用。
- 使用 `fetch` 请求 `GET /api/books/search`，支持 `q` 和 `letter` 查询参数。
- 兼容服务端返回原始数组或 `books`、`results`、`items` 三种结果字段。
- 图书卡片显示标题、作者、年份，并在服务端提供音频 URL 时显示可用播放按钮。
- 支持服务端反馈音频字段以及单本图书音频字段。
- 支持 `DAISY Book`、`Search`、`Play`、`Stop` 和 A-Z 字母命令。
- 浏览器支持 Web Speech API 时可使用语音输入；不支持时仍可使用键盘、按钮和原生音频控件。
- 添加桌面与移动端响应式布局、键盘 focus 状态、ARIA 状态播报和 reduced-motion 支持。
- 添加项目内插图 `frontend/public/daisy-audio-books.png`。

## API 约定

开发环境通过 Vite 将 `/api` 代理到 `VITE_API_TARGET`，默认值为 `http://localhost:8000`。详细响应示例见 `frontend/README.md`。

## 验证情况

- `npm install` 成功，未发现依赖漏洞。
- Vite build 在当前 Windows 环境创建 `node_modules/.vite-temp` 时遇到 `EPERM`。
- 用户选择自行编译，因此未继续请求提升权限，也未启动开发服务器或浏览器验证。

## 教训

音频搜索前端应兼容服务端不同响应包装形式，并让语音功能作为增强能力而不是唯一入口。原生音频控件、明确的播放/停止命令和键盘 focus 状态是基础可访问性要求。
