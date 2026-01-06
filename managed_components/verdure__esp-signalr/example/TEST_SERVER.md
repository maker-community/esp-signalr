# ASP.NET Core SignalR Test Server Setup

This guide shows how to create a test SignalR server for development and testing with ESP32 clients.

## Prerequisites

- .NET SDK 6.0 or later: https://dotnet.microsoft.com/download
- Visual Studio Code (recommended) or Visual Studio

## Quick Setup

### 1. Create New Project

```bash
# Create new ASP.NET Core Web App
dotnet new web -n SignalRTestServer
cd SignalRTestServer

# Add SignalR package
dotnet add package Microsoft.AspNetCore.SignalR
```

### 2. Create Hub Class

Create `Hubs/ChatHub.cs`:

```csharp
using Microsoft.AspNetCore.SignalR;

namespace SignalRTestServer.Hubs
{
    public class ChatHub : Hub
    {
        // Called when ESP32 sends a message
        public async Task SendMessage(string user, string message)
        {
            Console.WriteLine($"Received from {user}: {message}");
            
            // Broadcast to all connected clients
            await Clients.All.SendAsync("ReceiveMessage", user, message);
        }

        // Send notification to specific client
        public async Task SendToClient(string connectionId, string message)
        {
            await Clients.Client(connectionId).SendAsync("Notify", message);
        }

        // Echo back to sender
        public string Echo(string message)
        {
            return $"Echo: {message}";
        }

        // Update sensor data
        public async Task UpdateSensor(string sensorId, double value)
        {
            Console.WriteLine($"Sensor {sensorId}: {value}");
            await Clients.Others.SendAsync("SensorUpdate", sensorId, value);
        }

        // Connection lifecycle
        public override async Task OnConnectedAsync()
        {
            Console.WriteLine($"Client connected: {Context.ConnectionId}");
            await base.OnConnectedAsync();
        }

        public override async Task OnDisconnectedAsync(Exception? exception)
        {
            Console.WriteLine($"Client disconnected: {Context.ConnectionId}");
            if (exception != null)
            {
                Console.WriteLine($"Error: {exception.Message}");
            }
            await base.OnDisconnectedAsync(exception);
        }
    }
}
```

### 3. Configure Program.cs

Replace `Program.cs` content:

```csharp
using SignalRTestServer.Hubs;

var builder = WebApplication.CreateBuilder(args);

// Add SignalR services
builder.Services.AddSignalR();

// Configure CORS for ESP32 clients
builder.Services.AddCors(options =>
{
    options.AddDefaultPolicy(policy =>
    {
        policy.AllowAnyOrigin()
              .AllowAnyHeader()
              .AllowAnyMethod();
    });
});

var app = builder.Build();

// Enable CORS
app.UseCors();

// Map SignalR hub
app.MapHub<ChatHub>("/chatHub");

// Simple health check endpoint
app.MapGet("/", () => "SignalR Test Server is running!");

// Log server info
var urls = app.Urls;
foreach (var url in urls)
{
    Console.WriteLine($"Server listening on: {url}");
    Console.WriteLine($"SignalR Hub URL: {url}/chatHub");
}

app.Run();
```

### 4. Configure HTTPS (Optional but Recommended)

For development, you can use HTTP, but for production, use HTTPS.

**Development with HTTP:**

Modify `Properties/launchSettings.json`:

```json
{
  "profiles": {
    "SignalRTestServer": {
      "commandName": "Project",
      "dotnetRunMessages": true,
      "launchBrowser": false,
      "applicationUrl": "http://0.0.0.0:5000",
      "environmentVariables": {
        "ASPNETCORE_ENVIRONMENT": "Development"
      }
    }
  }
}
```

**Production with HTTPS:**

```bash
# Generate self-signed certificate
dotnet dev-certs https --trust

# Update applicationUrl
"applicationUrl": "https://0.0.0.0:5001;http://0.0.0.0:5000"
```

### 5. Run Server

```bash
dotnet run
```

Expected output:
```
info: Microsoft.Hosting.Lifetime[14]
      Now listening on: http://0.0.0.0:5000
Server listening on: http://0.0.0.0:5000
SignalR Hub URL: http://0.0.0.0:5000/chatHub
```

## Testing with curl

### Test Negotiate Endpoint

```bash
curl -X POST http://localhost:5000/chatHub/negotiate
```

Expected response:
```json
{
  "negotiateVersion": 1,
  "connectionId": "...",
  "availableTransports": [
    {
      "transport": "WebSockets",
      "transferFormats": ["Text","Binary"]
    }
  ]
}
```

## Advanced Features

### 1. Authentication

Add JWT authentication:

```csharp
// In Program.cs
builder.Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
    .AddJwtBearer(options =>
    {
        options.Events = new JwtBearerEvents
        {
            OnMessageReceived = context =>
            {
                var accessToken = context.Request.Query["access_token"];
                var path = context.HttpContext.Request.Path;
                
                if (!string.IsNullOrEmpty(accessToken) &&
                    path.StartsWithSegments("/chatHub"))
                {
                    context.Token = accessToken;
                }
                return Task.CompletedTask;
            }
        };
    });

app.UseAuthentication();
app.UseAuthorization();
```

In Hub:
```csharp
[Authorize]
public class ChatHub : Hub
{
    // Hub methods
}
```

### 2. Groups for Room-based Communication

```csharp
public class ChatHub : Hub
{
    public async Task JoinRoom(string roomName)
    {
        await Groups.AddToGroupAsync(Context.ConnectionId, roomName);
        await Clients.Group(roomName).SendAsync("UserJoined", Context.ConnectionId);
    }

    public async Task LeaveRoom(string roomName)
    {
        await Groups.RemoveFromGroupAsync(Context.ConnectionId, roomName);
        await Clients.Group(roomName).SendAsync("UserLeft", Context.ConnectionId);
    }

    public async Task SendToRoom(string roomName, string message)
    {
        await Clients.Group(roomName).SendAsync("ReceiveMessage", message);
    }
}
```

### 3. Streaming Data

```csharp
public class ChatHub : Hub
{
    // Server-to-client streaming
    public async IAsyncEnumerable<int> StreamCounter(int count, int delay)
    {
        for (var i = 0; i < count; i++)
        {
            yield return i;
            await Task.Delay(delay);
        }
    }
}
```

### 4. Strong Typing

```csharp
public interface IChatClient
{
    Task ReceiveMessage(string user, string message);
    Task SensorUpdate(string sensorId, double value);
}

public class ChatHub : Hub<IChatClient>
{
    public async Task SendMessage(string user, string message)
    {
        await Clients.All.ReceiveMessage(user, message);
    }
}
```

## ESP32 Client Configuration

Use this URL in your ESP32 code:

```cpp
#define SIGNALR_HUB_URL "http://your-server-ip:5000/chatHub"
// Or for HTTPS:
// #define SIGNALR_HUB_URL "https://your-server-ip:5001/chatHub"
```

## Docker Deployment (Optional)

### Dockerfile

```dockerfile
FROM mcr.microsoft.com/dotnet/aspnet:6.0 AS base
WORKDIR /app
EXPOSE 5000

FROM mcr.microsoft.com/dotnet/sdk:6.0 AS build
WORKDIR /src
COPY ["SignalRTestServer.csproj", "./"]
RUN dotnet restore "SignalRTestServer.csproj"
COPY . .
RUN dotnet build "SignalRTestServer.csproj" -c Release -o /app/build

FROM build AS publish
RUN dotnet publish "SignalRTestServer.csproj" -c Release -o /app/publish

FROM base AS final
WORKDIR /app
COPY --from=publish /app/publish .
ENTRYPOINT ["dotnet", "SignalRTestServer.dll"]
```

### Build and Run

```bash
docker build -t signalr-test-server .
docker run -p 5000:5000 signalr-test-server
```

## Monitoring and Debugging

### Enable Detailed Logging

In `appsettings.Development.json`:

```json
{
  "Logging": {
    "LogLevel": {
      "Default": "Information",
      "Microsoft.AspNetCore": "Warning",
      "Microsoft.AspNetCore.SignalR": "Debug",
      "Microsoft.AspNetCore.Http.Connections": "Debug"
    }
  }
}
```

### Add Request Logging Middleware

```csharp
app.Use(async (context, next) =>
{
    Console.WriteLine($"{context.Request.Method} {context.Request.Path}");
    await next();
});
```

## Common Issues

### Issue: CORS Errors
**Solution**: Ensure CORS is properly configured with `AllowAnyOrigin()`

### Issue: Connection Immediately Closes
**Solution**: Check firewall settings and ensure port 5000 is accessible

### Issue: ESP32 Can't Connect
**Solution**: 
1. Verify server is listening on `0.0.0.0` not `localhost`
2. Check ESP32 and server are on same network
3. Try HTTP first before HTTPS

## Production Deployment

For production, consider:

1. **Use HTTPS with valid certificates**
2. **Configure proper CORS policies** (don't use `AllowAnyOrigin`)
3. **Add authentication and authorization**
4. **Use Azure SignalR Service** for scaling
5. **Implement health checks**
6. **Add structured logging**

### Azure SignalR Service

```csharp
builder.Services.AddSignalR()
    .AddAzureSignalR("<connection-string>");
```

## Testing Tools

### SignalR Client Test Tool

```bash
npm install -g @microsoft/signalr
```

Create `test-client.js`:

```javascript
const signalR = require("@microsoft/signalr");

const connection = new signalR.HubConnectionBuilder()
    .withUrl("http://localhost:5000/chatHub")
    .build();

connection.on("ReceiveMessage", (user, message) => {
    console.log(`${user}: ${message}`);
});

connection.start()
    .then(() => {
        console.log("Connected!");
        return connection.invoke("SendMessage", "TestClient", "Hello from Node.js!");
    })
    .catch(err => console.error(err));
```

Run:
```bash
node test-client.js
```

## Resources

- ASP.NET Core SignalR Documentation: https://learn.microsoft.com/aspnet/core/signalr/
- Azure SignalR Service: https://azure.microsoft.com/services/signalr-service/
- SignalR GitHub: https://github.com/dotnet/aspnetcore/tree/main/src/SignalR

## Next Steps

1. Customize hub methods for your use case
2. Test with ESP32 client
3. Add authentication if needed
4. Deploy to cloud service for remote access
