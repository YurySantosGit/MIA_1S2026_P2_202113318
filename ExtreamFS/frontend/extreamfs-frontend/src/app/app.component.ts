import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { BackendService, ExecuteResponse, HealthResponse } from './backend.service';

@Component({
  selector: 'app-root',
  standalone: true,
  imports: [CommonModule, FormsModule],
  templateUrl: './app.component.html',
  styleUrl: './app.component.css'
})
export class AppComponent implements OnInit {
  title = 'ExtreamFS Frontend';

  commandInput: string = '';
  outputText: string = 'Consola lista.\n';
  selectedFileName: string = '';
  selectedFileContent: string = '';

  backendStatus: string = 'Verificando...';

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

  onFileSelected(event: Event): void {
    const input = event.target as HTMLInputElement;

    if (!input.files || input.files.length === 0) {
      return;
    }

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
      },
      error: () => {
        this.outputText += `[ERROR] No se pudo conectar con el backend.\n`;
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
      },
      error: () => {
        this.outputText += `[ERROR] No se pudo conectar con el backend.\n`;
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
    if (input) {
      input.value = '';
    }
  }
}