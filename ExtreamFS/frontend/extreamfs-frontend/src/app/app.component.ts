import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import {
  BackendService,
  ExecuteResponse,
  HealthResponse,
  DiskItem,
  PartitionItem,
  FileSystemResponse
} from './backend.service';

interface FsItem {
  name: string;
  path: string;
  isFile: boolean;
  level: number;
}

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './app.component.html',
  styleUrl: './app.component.css'
})
export class AppComponent implements OnInit {
  title = 'ExtreamFS Frontend';

  currentView: 'home' | 'login' | 'visualizer' = 'home';

  commandInput: string = '';
  outputText: string = 'Consola lista.\n';
  selectedFileName: string = '';
  selectedFileContent: string = '';

  backendStatus: string = 'Verificando...';

  loginUser: string = 'root';
  loginPass: string = '123';
  loginId: string = '181A';
  sessionActive: boolean = false;

  disks: DiskItem[] = [];
  selectedDisk: DiskItem | null = null;

  partitions: PartitionItem[] = [];
  selectedPartition: PartitionItem | null = null;

  fsOutput: string = '';
  fsPath: string = '/';
  fsItems: FsItem[] = [];

  selectedFilePath: string = '';
  selectedFileContentViewer: string = '';

  visualizerMessage: string = 'Seleccione un disco para continuar.';

  constructor(private backendService: BackendService) {}

  ngOnInit(): void {
    this.checkBackendStatus();
  }

  checkBackendStatus(): void {
    this.backendStatus = 'Verificando...';

    this.backendService.checkHealth().subscribe({
      next: (response: HealthResponse) => {
        this.backendStatus = response.status || 'Conectado';
      },
      error: () => {
        this.backendStatus = 'Desconectado';
      }
    });
  }

  goToLogin(): void {
    this.currentView = 'login';
  }

  goToHome(): void {
    this.currentView = 'home';
  }

  goToVisualizer(): void {
    this.currentView = 'visualizer';
    this.loadDisks();
  }

  loadDisks(): void {
    this.visualizerMessage = 'Cargando discos...';
    this.selectedDisk = null;
    this.selectedPartition = null;
    this.partitions = [];
    this.fsOutput = '';
    this.fsItems = [];

    this.backendService.getDisks().subscribe({
      next: (response) => {
        this.disks = response.disks || [];

        if (this.disks.length === 0) {
          this.visualizerMessage = 'No se encontraron discos .mia.';
        } else {
          this.visualizerMessage = 'Discos encontrados: ' + this.disks.length;
        }
      },
      error: (err) => {
        this.visualizerMessage = 'No se pudieron cargar los discos.';
        this.outputText += `[ERROR] No se pudieron cargar discos. Status: ${err.status}\n`;
      }
    });
  }

  selectDisk(disk: DiskItem): void {
    this.selectedDisk = disk;
    this.selectedPartition = null;
    this.partitions = [];
    this.fsOutput = '';
    this.fsItems = [];
    this.visualizerMessage = `Disco seleccionado: ${disk.name}`;
    this.loadPartitions(disk.path);
  }

  loadPartitions(diskPath: string): void {
    this.visualizerMessage = 'Cargando particiones...';

    this.backendService.getPartitions(diskPath).subscribe({
      next: (response) => {
        this.partitions = response.partitions || [];

        if (this.partitions.length === 0) {
          this.visualizerMessage = 'El disco seleccionado no tiene particiones activas.';
        } else {
          this.visualizerMessage = `Particiones encontradas: ${this.partitions.length}`;
        }
      },
      error: (err) => {
        this.visualizerMessage = 'No se pudieron cargar las particiones.';
        this.outputText += `[ERROR] No se pudieron cargar particiones. Status: ${err.status}\n`;
      }
    });
  }

  selectPartition(partition: PartitionItem): void {
    this.selectedPartition = partition;
    this.visualizerMessage = `Partición seleccionada: ${partition.name}`;
    this.loadFileSystem('/');
  }

  loadFileSystem(path: string = '/'): void {
    this.fsPath = path;
    this.fsOutput = 'Cargando sistema de archivos...';
    this.fsItems = [];
    this.selectedFilePath = '';
    this.selectedFileContentViewer = '';

    this.backendService.getFileSystem(path).subscribe({
      next: (response: FileSystemResponse) => {
        if (!response.success) {
          this.fsOutput = response.output || '[ERROR] No se pudo cargar el sistema de archivos.';
          this.fsItems = [];
          return;
        }

        this.fsPath = response.path || path;
        this.fsOutput = response.output || '';
        this.fsItems = this.parseFsOutput(this.fsOutput);
      },
      error: (err) => {
        this.fsOutput = `[ERROR] No se pudo cargar el sistema de archivos. Status: ${err.status}`;
        this.fsItems = [];
      }
    });
  }

  parseFsOutput(output: string): FsItem[] {
    const lines = output.split('\n');
    const items: FsItem[] = [];
    const stack: string[] = [];

    for (const line of lines) {
      if (!line.includes('|_')) continue;

      const rawName = line.split('|_')[1]?.trim();
      if (!rawName) continue;

      const spacesBeforeMarker = line.indexOf('|_');
      const level = Math.floor(spacesBeforeMarker / 3);

      stack[level] = rawName;
      stack.length = level + 1;

      const fullPath = '/' + stack.join('/');
      const isFile = rawName.includes('.');

      items.push({
        name: rawName,
        path: fullPath,
        isFile,
        level
      });
    }

    return items;
  }

  openFileContent(path: string): void {
    this.selectedFilePath = path;
    this.selectedFileContentViewer = 'Cargando contenido del archivo...';

    const command = `cat -file1=${path}`;

    this.backendService.executeCommands(command).subscribe({
      next: (response: ExecuteResponse) => {
        if (!response.success) {
          this.selectedFileContentViewer = response.output || '[ERROR] No se pudo leer el archivo.';
          return;
        }

        this.selectedFileContentViewer =
          `Ruta: ${path}\n\n` + (response.output || '[INFO] Archivo vacío.');
      },
      error: (err) => {
        this.selectedFileContentViewer =
          `[ERROR] No se pudo cargar el archivo. Status: ${err.status}`;
      }
    });
  }

  openFsItem(item: FsItem): void {
    if (item.isFile) {
      this.openFileContent(item.path);
      return;
    }

    this.selectedFilePath = '';
    this.selectedFileContentViewer = '';
    this.loadFileSystem(item.path);
  }

  

  formatBytes(bytes: number): string {
    if (bytes >= 1024 * 1024) {
      return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
    }

    if (bytes >= 1024) {
      return (bytes / 1024).toFixed(2) + ' KB';
    }

    return bytes + ' B';
  }

  formatPartitionType(type: string): string {
    const t = type.toLowerCase();

    if (t === 'p') return 'Primaria';
    if (t === 'e') return 'Extendida';
    if (t === 'l') return 'Lógica';

    return type;
  }

  goUpDirectory(): void {
    if (this.fsPath === '/') return;

    const parts = this.fsPath.split('/').filter(p => p.length > 0);
    parts.pop();

    const newPath = '/' + parts.join('/');
    this.loadFileSystem(newPath === '/' ? '/' : newPath);
  }

  onFileSelected(event: Event): void {
    const input = event.target as HTMLInputElement;

    if (!input.files || input.files.length === 0) return;

    const file = input.files[0];
    this.selectedFileName = file.name;

    const reader = new FileReader();

    reader.onload = () => {
      this.selectedFileContent = String(reader.result || '');
      this.outputText += `[INFO] Archivo cargado: ${this.selectedFileName}\n`;
    };

    reader.onerror = () => {
      this.outputText += `[ERROR] No se pudo leer el archivo.\n`;
    };

    reader.readAsText(file);
  }

  loginGraphic(): void {
    const user = this.loginUser.trim();
    const pass = this.loginPass.trim();
    const id = this.loginId.trim();

    if (!user || !pass || !id) {
      this.outputText += `[ERROR] Debe ingresar usuario, contraseña e id.\n`;
      return;
    }

    const command = `login -user=${user} -pass=${pass} -id=${id}`;

    this.outputText += `\n[INFO] Iniciando sesión gráfica...\n`;

    this.backendService.executeCommands(command).subscribe({
      next: (response: ExecuteResponse) => {
        this.outputText += `${response.output}\n`;

        if (response.output.includes('[OK] Sesion iniciada correctamente')) {
          this.sessionActive = true;
          this.currentView = 'home';
        }
      },
      error: (err) => {
        this.outputText += `[ERROR] Fallo en login gráfico\n`;
        this.outputText += `Status: ${err.status}\n`;
        this.outputText += `Respuesta: ${JSON.stringify(err.error)}\n`;
      }
    });
  }

  logoutGraphic(): void {
    this.outputText += `\n[INFO] Cerrando sesión...\n`;

    this.backendService.executeCommands('logout').subscribe({
      next: (response: ExecuteResponse) => {
        this.outputText += `${response.output}\n`;

        if (response.output.includes('[OK] Sesion cerrada correctamente')) {
          this.sessionActive = false;
          this.fsOutput = '';
          this.fsItems = [];
        }
      },
      error: (err) => {
        this.outputText += `[ERROR] Fallo en logout\n`;
        this.outputText += `Status: ${err.status}\n`;
        this.outputText += `Respuesta: ${JSON.stringify(err.error)}\n`;
      }
    });
  }

  executeCommands(): void {
    const contentToExecute = this.commandInput.trim();

    if (!contentToExecute) {
      this.outputText += `[ERROR] No hay comandos para ejecutar en la entrada manual.\n`;
      return;
    }

    this.outputText += `\n[INFO] Ejecutando comandos manuales...\n`;

    this.backendService.executeCommands(contentToExecute).subscribe({
      next: (response: ExecuteResponse) => {
        this.outputText += `${response.output}\n`;

        if (response.output.includes('[OK] Sesion iniciada correctamente')) {
          this.sessionActive = true;
        }

        if (response.output.includes('[OK] Sesion cerrada correctamente')) {
          this.sessionActive = false;
          this.fsOutput = '';
          this.fsItems = [];
        }
      },
      error: (err) => {
        this.outputText += `[ERROR] Fallo en backend\n`;
        this.outputText += `Status: ${err.status}\n`;
        this.outputText += `Mensaje: ${err.message}\n`;
        this.outputText += `Respuesta: ${JSON.stringify(err.error)}\n\n`;
      }
    });
  }

  executeLoadedFile(): void {
    const contentToExecute = this.selectedFileContent.trim();

    if (!contentToExecute) {
      this.outputText += `[ERROR] No hay archivo cargado para ejecutar.\n`;
      return;
    }

    this.outputText += `\n[INFO] Ejecutando archivo cargado...\n`;

    this.backendService.executeCommands(contentToExecute).subscribe({
      next: (response: ExecuteResponse) => {
        this.outputText += `${response.output}\n`;

        if (response.output.includes('[OK] Sesion iniciada correctamente')) {
          this.sessionActive = true;
        }

        if (response.output.includes('[OK] Sesion cerrada correctamente')) {
          this.sessionActive = false;
          this.fsOutput = '';
          this.fsItems = [];
        }
      },
      error: (err) => {
        this.outputText += `[ERROR] Fallo en backend\n`;
        this.outputText += `Status: ${err.status}\n`;
        this.outputText += `Mensaje: ${err.message}\n`;
        this.outputText += `Respuesta: ${JSON.stringify(err.error)}\n\n`;
      }
    });
  }

  clearCommandInput(): void {
    this.commandInput = '';
  }

  clearLoadedFile(): void {
    this.selectedFileName = '';
    this.selectedFileContent = '';

    const input = document.getElementById('fileInput') as HTMLInputElement | null;
    if (input) input.value = '';
  }
}